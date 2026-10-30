//
// Phase 0 采集探针：在写屏幕共享业务代码之前，先确认真实采集帧长什么样。
//
// 它回答四个问题：
//   1. 屏幕/窗口采集在当前平台给出的 pixelFormat / handleType / planeCount 是什么；
//   2. QVideoFrame 能否 map / toImage，行距与尺寸是否正常；
//   3. videoFrameChanged 在哪个线程回调，是否在 GUI 线程；
//   4. 系统声音的 monitor source 是否能被 libpulse 看到（Qt 与 ADM 都会过滤它）。
//
// 它不链接 WebRTC，也不进 CTest：需要真实图形会话，且会实际抓屏。
//
// 用法：
//   ./syncine_capture_probe --list
//   ./syncine_capture_probe --screen 0 --duration 5
//   ./syncine_capture_probe --window 3 --duration 5
//   ./syncine_capture_probe --window-title Chrome --duration 5
//   ./syncine_capture_probe --selftest --duration 5
//   ./syncine_capture_probe --audio
//
// --selftest 会弹出一个四色块窗口，再抓这个窗口本身，用已知颜色验证
// 通道顺序与上下翻转；不需要抓桌面内容，适合第一次验证。
//
// --save <path> 会把第一帧写成 PNG。屏幕内容属于敏感数据，默认不保存，
// 只在显式传参时写盘。
//

#include <QAudioDevice>
#include <QAudioFormat>
#include <QCapturableWindow>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QRasterWindow>
#include <QScreen>
#include <QScreenCapture>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>
#include <QWindow>
#include <QWindowCapture>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>

#ifdef SYNCINE_PROBE_HAVE_PULSE
#include <pulse/pulseaudio.h>
#endif

namespace {

struct Options {
    enum class Mode {
        Screen,
        Window,
        SelfTest,
    };

    Mode mode = Mode::Screen;
    int screenIndex = 0;
    int windowIndex = -1;
    QString windowTitle;
    int durationSec = 5;
    QString savePath;
};

QString threadLabel()
{
    QThread *thread = QThread::currentThread();
    const bool gui = thread == QCoreApplication::instance()->thread();
    QString name = thread->objectName();
    if (name.isEmpty())
        name = QStringLiteral("unnamed");
    return QStringLiteral("%1[%2,0x%3]")
        .arg(gui ? QStringLiteral("GUI") : QStringLiteral("worker"))
        .arg(name)
        .arg(reinterpret_cast<quintptr>(thread), 0, 16);
}

QString pixelFormatName(const QVideoFrameFormat &format)
{
    return QVideoFrameFormat::pixelFormatToString(format.pixelFormat());
}

// 只对 NoHandle 的 CPU 帧调用，避免在错误线程碰 GPU 纹理。
QString imageDescription(const QVideoFrame &frame)
{
    if (frame.handleType() != QVideoFrame::NoHandle)
        return QStringLiteral("skipped(gpu)");

    const QImage image = frame.toImage();
    if (image.isNull())
        return QStringLiteral("failed");
    return QStringLiteral("ok(%1x%2, QImage::Format=%3)")
        .arg(image.width())
        .arg(image.height())
        .arg(static_cast<int>(image.format()));
}

// 四色块窗口：只用于 --selftest，不需要抓用户桌面。
class ColorBarWindow : public QRasterWindow
{
public:
    ColorBarWindow()
    {
        setTitle(QStringLiteral("Syncine Capture Probe Color Bars"));
        resize(640, 480);
        setFlag(Qt::WindowStaysOnTopHint); // 避免被终端挡住
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const int w = width();
        const int h = height();

        painter.fillRect(QRect(0, 0, w / 2, h / 2), QColor(255, 0, 0));
        painter.fillRect(QRect(w / 2, 0, w - w / 2, h / 2), QColor(0, 255, 0));
        painter.fillRect(QRect(0, h / 2, w / 2, h - h / 2), QColor(0, 0, 255));
        painter.fillRect(QRect(w / 2, h / 2, w - w / 2, h - h / 2),
                         QColor(255, 255, 255));

        painter.setPen(Qt::black);
        painter.drawText(QRect(0, 0, w, h), Qt::AlignCenter,
                         QStringLiteral("Syncine capture probe"));
    }
};

class CaptureProbe : public QObject
{
public:
    explicit CaptureProbe(Options options, QObject *parent = nullptr)
        : QObject(parent), m_options(std::move(options))
    {
        connect(&m_sink, &QVideoSink::videoFrameChanged, this,
                [this](const QVideoFrame &frame) { onFrame(frame); });

        connect(&m_screen, &QScreenCapture::errorOccurred, this,
                [this](QScreenCapture::Error error, const QString &text) {
                    onError(QStringLiteral("screen capture error %1: %2")
                                .arg(static_cast<int>(error)).arg(text));
                });
        connect(&m_window, &QWindowCapture::errorOccurred, this,
                [this](QWindowCapture::Error error, const QString &text) {
                    onError(QStringLiteral("window capture error %1: %2")
                                .arg(static_cast<int>(error)).arg(text));
                });
    }

    ~CaptureProbe() override
    {
        cleanupSession();
    }

    bool start()
    {
        switch (m_options.mode) {
        case Options::Mode::Screen:
            return startScreen();
        case Options::Mode::Window:
            return startWindow();
        case Options::Mode::SelfTest:
            return startSelfTest();
        }
        return false;
    }

private:
    struct Stats {
        int count = 0;
        qint64 firstNs = 0;
        qint64 lastNs = 0;
        qint64 minIntervalNs = std::numeric_limits<qint64>::max();
        qint64 maxIntervalNs = 0;
        qint64 totalIntervalNs = 0;
        int intervalCount = 0;
        QHash<QString, int> formats;
        QHash<int, int> handles;
        QSet<QString> threads;
        int mapOk = 0;
        int mapFailed = 0;
        int mapSkippedGpu = 0;
        int imageOk = 0;
        int imageFailed = 0;
        QSize lastSize;
        int sizeChanges = 0;
    };

    bool startScreen()
    {
        const QList<QScreen *> screens = QGuiApplication::screens();
        if (screens.isEmpty()) {
            qWarning().noquote() << "[probe] 没有可用屏幕";
            return false;
        }
        if (m_options.screenIndex < 0 || m_options.screenIndex >= screens.size()) {
            qWarning().noquote() << "[probe] 屏幕索引越界:" << m_options.screenIndex
                                 << "可用数量:" << screens.size();
            return false;
        }

        QScreen *screen = screens.at(m_options.screenIndex);
        qInfo().noquote() << "[probe] 开始采集屏幕" << m_options.screenIndex
                          << screen->name() << screen->geometry()
                          << "dpr=" << screen->devicePixelRatio()
                          << "refresh=" << screen->refreshRate();

        m_selftest = false;
        m_session.setScreenCapture(&m_screen);
        m_screen.setScreen(screen);
        m_session.setVideoSink(&m_sink);

        m_elapsed.start();
        m_screen.start();
        if (m_screen.error() != QScreenCapture::NoError) {
            onError(QStringLiteral("screen start error %1: %2")
                        .arg(static_cast<int>(m_screen.error()))
                        .arg(m_screen.errorString()));
            return false;
        }

        scheduleStop();
        return true;
    }

    bool startWindow()
    {
        const QList<QCapturableWindow> windows = QWindowCapture::capturableWindows();
        if (windows.isEmpty()) {
            qWarning().noquote() << "[probe] 当前平台没有可枚举的窗口（Wayland 下常见）";
            return false;
        }

        int index = m_options.windowIndex;
        if (!m_options.windowTitle.isEmpty()) {
            index = -1;
            for (int i = 0; i < windows.size(); ++i) {
                if (windows.at(i).description().contains(m_options.windowTitle,
                                                         Qt::CaseInsensitive)) {
                    index = i;
                    break;
                }
            }
            if (index < 0) {
                qWarning().noquote() << "[probe] 没有找到标题包含"
                                     << m_options.windowTitle << "的窗口";
                return false;
            }
        }
        if (index < 0 || index >= windows.size()) {
            qWarning().noquote() << "[probe] 窗口索引越界:" << index
                                 << "可用数量:" << windows.size();
            return false;
        }

        const QCapturableWindow window = windows.at(index);
        qInfo().noquote() << "[probe] 开始采集窗口" << index
                          << window.description();

        m_selftest = false;
        m_session.setWindowCapture(&m_window);
        m_window.setWindow(window);
        m_session.setVideoSink(&m_sink);

        m_elapsed.start();
        m_window.start();
        if (m_window.error() != QWindowCapture::NoError) {
            onError(QStringLiteral("window start error %1: %2")
                        .arg(static_cast<int>(m_window.error()))
                        .arg(m_window.errorString()));
            return false;
        }

        scheduleStop();
        return true;
    }

    bool startSelfTest()
    {
        m_selftest = true;
        m_colorWindow.reset(new ColorBarWindow);
        m_colorWindow->show();
        m_colorWindow->raise();
        m_colorWindow->requestActivate();

        qInfo().noquote() << "[probe] 已显示四色块窗口，等待 700ms 后开始抓取该窗口";

        QTimer::singleShot(700, this, [this] {
            QCapturableWindow target;
            const QList<QCapturableWindow> windows =
                QWindowCapture::capturableWindows();
            for (const QCapturableWindow &window : windows) {
                if (window.description().contains(
                        QStringLiteral("Capture Probe Color Bars"))) {
                    target = window;
                    break;
                }
            }
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
            if (!target.isValid())
                target = QCapturableWindow(m_colorWindow.get());
#endif
            if (!target.isValid()) {
                onError(QStringLiteral("找不到用于自检的窗口"));
                return;
            }

            m_session.setWindowCapture(&m_window);
            m_window.setWindow(target);
            m_session.setVideoSink(&m_sink);

            m_elapsed.start();
            m_window.start();
            if (m_window.error() != QWindowCapture::NoError) {
                onError(QStringLiteral("selftest start error %1: %2")
                            .arg(static_cast<int>(m_window.error()))
                            .arg(m_window.errorString()));
                return;
            }

            scheduleStop();
        });
        return true;
    }

    void scheduleStop()
    {
        QTimer::singleShot(m_options.durationSec * 1000, this, [this] { stop(); });
    }

    void stop()
    {
        if (m_stopped)
            return;
        m_stopped = true;

        m_screen.stop();
        m_window.stop();

        // 让 stop() 之后仍可能在队列里的最后一两帧走完统计，再输出汇总。
        QTimer::singleShot(200, this, [this] {
            printSummary();
            if (m_colorWindow)
                m_colorWindow->close();
            if (m_stats.count == 0 && m_exitCode == 0)
                m_exitCode = 3;
            QCoreApplication::exit(m_exitCode);
        });
    }

    void onError(const QString &message)
    {
        // errorOccurred 可能来自采集后端线程；QScreenCapture/QWindowCapture
        // 的 stop 必须在它们所属的（GUI）线程执行。
        if (QThread::currentThread() != thread()) {
            QMetaObject::invokeMethod(this, [this, message] { onError(message); },
                                      Qt::QueuedConnection);
            return;
        }

        qWarning().noquote() << "[probe]" << message;
        m_exitCode = 2;
        stop();
    }

    void onFrame(const QVideoFrame &frame)
    {
        QMutexLocker locker(&m_statsMutex);
        if (m_stopped)
            return;

        const qint64 nowNs = m_elapsed.nsecsElapsed();
        const QSize size(frame.width(), frame.height());

        if (m_stats.count == 0) {
            m_stats.firstNs = nowNs;
        } else {
            const qint64 interval = nowNs - m_stats.lastNs;
            m_stats.minIntervalNs = std::min(m_stats.minIntervalNs, interval);
            m_stats.maxIntervalNs = std::max(m_stats.maxIntervalNs, interval);
            m_stats.totalIntervalNs += interval;
            ++m_stats.intervalCount;
        }
        m_stats.lastNs = nowNs;

        if (size != m_stats.lastSize) {
            if (m_stats.lastSize.isValid())
                ++m_stats.sizeChanges;
            m_stats.lastSize = size;
        }

        const QVideoFrameFormat surface = frame.surfaceFormat();
        ++m_stats.formats[pixelFormatName(surface)];
        ++m_stats.handles[static_cast<int>(frame.handleType())];
        m_stats.threads.insert(threadLabel());

        const bool verbose = (m_stats.count < 3) || (m_stats.count % 30 == 0);
        inspectFrame(frame, m_stats.count + 1, nowNs, verbose);

        // 自检：第 10 帧做一次颜色采样（前几帧可能还没稳定）。
        if (m_selftest && m_stats.count == 9)
            sampleColors(frame);

        if (!m_options.savePath.isEmpty() && !m_saveTried)
            saveFirstFrame(frame);

        ++m_stats.count;
    }

    void inspectFrame(const QVideoFrame &frame, int index, qint64 nowNs, bool verbose)
    {
        const QVideoFrameFormat surface = frame.surfaceFormat();

        QString mapText;
        QVideoFrame mapped = frame;
        if (frame.handleType() != QVideoFrame::NoHandle) {
            mapText = QStringLiteral("skipped(gpu)");
            ++m_stats.mapSkippedGpu;
        } else if (!mapped.map(QVideoFrame::ReadOnly)) {
            mapText = QStringLiteral("failed");
            ++m_stats.mapFailed;
        } else {
            mapText = QStringLiteral("ok(planes=%1").arg(mapped.planeCount());
            for (int plane = 0; plane < mapped.planeCount(); ++plane) {
                mapText += QStringLiteral(", stride%1=%2")
                               .arg(plane)
                               .arg(mapped.bytesPerLine(plane));
            }
            mapText += QLatin1Char(')');
            ++m_stats.mapOk;
            mapped.unmap();
        }

        QString imageText = QStringLiteral("skipped");
        if (verbose) {
            imageText = imageDescription(frame);
            if (imageText.startsWith(QLatin1String("ok")))
                ++m_stats.imageOk;
            else if (imageText == QLatin1String("failed"))
                ++m_stats.imageFailed;
        }

        if (!verbose)
            return;

        qInfo().noquote()
            << "[probe] frame" << index
            << "t=" << (nowNs / 1000000) << "ms"
            << "thread=" << threadLabel()
            << "size=" << frame.width() << "x" << frame.height()
            << "pixfmt=" << static_cast<int>(surface.pixelFormat())
            << pixelFormatName(surface)
            << "handle=" << static_cast<int>(frame.handleType())
            << "planes=" << frame.planeCount()
            << "rotation=" << static_cast<int>(surface.rotation())
            << "mirrored=" << surface.isMirrored()
            << "scanline=" << static_cast<int>(surface.scanLineDirection())
            << "sinkRhi=" << (m_sink.rhi() != nullptr)
            << "map=" << mapText
            << "toImage=" << imageText;
    }

    void sampleColors(const QVideoFrame &frame)
    {
        const QImage image = frame.toImage();
        if (image.isNull()) {
            qWarning().noquote() << "[probe] selftest: toImage() 失败，无法采样颜色";
            return;
        }

        struct Quadrant {
            const char *name;
            double fx;
            double fy;
            QColor expected;
        };
        const Quadrant quadrants[] = {
            {"top-left", 0.25, 0.25, QColor(255, 0, 0)},
            {"top-right", 0.75, 0.25, QColor(0, 255, 0)},
            {"bottom-left", 0.25, 0.75, QColor(0, 0, 255)},
            {"bottom-right", 0.75, 0.75, QColor(255, 255, 255)},
        };

        qInfo().noquote() << "[probe] selftest 颜色采样，图像"
                          << image.size()
                          << "QImage::Format=" << static_cast<int>(image.format());

        QVideoFrame mapped = frame;
        const bool canMap = frame.handleType() == QVideoFrame::NoHandle
                            && mapped.map(QVideoFrame::ReadOnly);

        for (const Quadrant &q : quadrants) {
            const int x = static_cast<int>(image.width() * q.fx);
            const int y = static_cast<int>(image.height() * q.fy);
            const QColor actual = image.pixelColor(x, y);
            const int dr = qAbs(actual.red() - q.expected.red());
            const int dg = qAbs(actual.green() - q.expected.green());
            const int db = qAbs(actual.blue() - q.expected.blue());
            const bool ok = dr <= 12 && dg <= 12 && db <= 12;

            QString raw = QStringLiteral("skipped");
            if (canMap && mapped.planeCount() > 0 && mapped.bits(0) != nullptr
                && x < mapped.width() && y < mapped.height()) {
                // mapped 已经 map 过，这里直接读，不重复 map。
                const QVideoFrameFormat::PixelFormat format =
                    mapped.surfaceFormat().pixelFormat();
                const int bpl = mapped.bytesPerLine(0);
                const uchar *row = mapped.bits(0)
                                   + static_cast<qint64>(y) * bpl;
                const uchar *pixel = row + static_cast<qint64>(x) * 4;
                switch (format) {
                case QVideoFrameFormat::Format_BGRA8888:
                case QVideoFrameFormat::Format_BGRA8888_Premultiplied:
                case QVideoFrameFormat::Format_BGRX8888:
                    raw = QStringLiteral("B=%1 G=%2 R=%3")
                              .arg(pixel[0]).arg(pixel[1]).arg(pixel[2]);
                    break;
                case QVideoFrameFormat::Format_RGBA8888:
                case QVideoFrameFormat::Format_RGBX8888:
                    raw = QStringLiteral("R=%1 G=%2 B=%3")
                              .arg(pixel[0]).arg(pixel[1]).arg(pixel[2]);
                    break;
                case QVideoFrameFormat::Format_ARGB8888:
                case QVideoFrameFormat::Format_ARGB8888_Premultiplied:
                case QVideoFrameFormat::Format_XRGB8888:
                    raw = QStringLiteral("A/X=%1 R=%2 G=%3 B=%4")
                              .arg(pixel[0]).arg(pixel[1]).arg(pixel[2]).arg(pixel[3]);
                    break;
                case QVideoFrameFormat::Format_ABGR8888:
                case QVideoFrameFormat::Format_XBGR8888:
                    raw = QStringLiteral("A/X=%1 B=%2 G=%3 R=%4")
                              .arg(pixel[0]).arg(pixel[1]).arg(pixel[2]).arg(pixel[3]);
                    break;
                default:
                    raw = QStringLiteral("non-RGB");
                    break;
                }
            }

            qInfo().noquote()
                << "[probe] selftest" << q.name
                << "expected=" << q.expected.name(QColor::HexRgb)
                << "actual=" << actual.name(QColor::HexRgb)
                << (ok ? "OK" : "MISMATCH")
                << "raw" << raw;
        }

        if (canMap)
            mapped.unmap();
    }

    void saveFirstFrame(const QVideoFrame &frame)
    {
        m_saveTried = true;
        if (frame.handleType() != QVideoFrame::NoHandle) {
            qWarning().noquote() << "[probe] --save: GPU 帧暂不尝试 toImage";
            return;
        }
        const QImage image = frame.toImage();
        if (image.isNull()) {
            qWarning().noquote() << "[probe] --save: toImage() 失败";
            return;
        }
        if (image.save(m_options.savePath)) {
            qInfo().noquote() << "[probe] 已保存首帧到" << m_options.savePath
                              << "（包含屏幕内容，请自行保管）";
        } else {
            qWarning().noquote() << "[probe] --save: 写入失败" << m_options.savePath;
        }
    }

    void printSummary()
    {
        QMutexLocker locker(&m_statsMutex);
        qInfo().noquote() << "[probe] ---- summary ----";
        if (m_stats.count == 0) {
            qWarning().noquote() << "[probe] 没有收到任何帧";
            return;
        }

        const double spanSeconds =
            static_cast<double>(m_stats.lastNs - m_stats.firstNs) / 1e9;
        const double fps = spanSeconds > 0.0
                               ? (m_stats.count - 1) / spanSeconds
                               : 0.0;
        qInfo().noquote() << "[probe] frames=" << m_stats.count
                          << "span=" << QString::number(spanSeconds, 'f', 2) << "s"
                          << "avgFps=" << QString::number(fps, 'f', 2);

        if (m_stats.intervalCount > 0) {
            qInfo().noquote()
                << "[probe] interval ms: min="
                << QString::number(m_stats.minIntervalNs / 1e6, 'f', 2)
                << "avg="
                << QString::number(m_stats.totalIntervalNs / 1e6
                                       / m_stats.intervalCount, 'f', 2)
                << "max="
                << QString::number(m_stats.maxIntervalNs / 1e6, 'f', 2);
        }

        qInfo().noquote() << "[probe] pixel formats:";
        for (auto it = m_stats.formats.cbegin(); it != m_stats.formats.cend(); ++it)
            qInfo().noquote() << "[probe]   " << it.key() << "=" << it.value();

        qInfo().noquote() << "[probe] handle types:";
        for (auto it = m_stats.handles.cbegin(); it != m_stats.handles.cend(); ++it)
            qInfo().noquote() << "[probe]   " << it.key() << "=" << it.value();

        qInfo().noquote() << "[probe] callback threads:"
                          << m_stats.threads.values();
        qInfo().noquote() << "[probe] map: ok=" << m_stats.mapOk
                          << "failed=" << m_stats.mapFailed
                          << "skipped-gpu=" << m_stats.mapSkippedGpu;
        qInfo().noquote() << "[probe] toImage(sampled frames): ok="
                          << m_stats.imageOk
                          << "failed=" << m_stats.imageFailed;
        qInfo().noquote() << "[probe] size=" << m_stats.lastSize
                          << "sizeChanges=" << m_stats.sizeChanges;

        if (m_stats.handles.contains(0) == false
            && !m_stats.handles.isEmpty()) {
            qInfo().noquote()
                << "[probe] 注意：所有帧都是 GPU handle，当前 VideoTrackSource 的"
                   " worker 转换路径无法直接 map；需要 GUI 回退或 toImage 回退。";
        }
    }

    void cleanupSession()
    {
        m_screen.stop();
        m_window.stop();
        m_session.setScreenCapture(nullptr);
        m_session.setWindowCapture(nullptr);
        m_session.setVideoSink(nullptr);
    }

    Options m_options;
    QMediaCaptureSession m_session;
    QScreenCapture m_screen;
    QWindowCapture m_window;
    QVideoSink m_sink;

    QElapsedTimer m_elapsed;
    Stats m_stats;
    bool m_selftest = false;
    std::atomic<bool> m_stopped{false};
    QMutex m_statsMutex;
    bool m_saveTried = false;
    int m_exitCode = 0;
    std::unique_ptr<ColorBarWindow> m_colorWindow;
};

void listScreensAndWindows()
{
    qInfo().noquote() << "[probe] Qt" << qVersion()
                      << "platform=" << QGuiApplication::platformName()
                      << "session=" << qgetenv("XDG_SESSION_TYPE");

    const QList<QScreen *> screens = QGuiApplication::screens();
    qInfo().noquote() << "[probe] screens:" << screens.size();
    for (int i = 0; i < screens.size(); ++i) {
        const QScreen *screen = screens.at(i);
        qInfo().noquote() << "[probe]  [" << i << "]"
                          << screen->name()
                          << screen->geometry()
                          << "dpr=" << screen->devicePixelRatio()
                          << "refresh=" << screen->refreshRate();
    }

    const QList<QCapturableWindow> windows =
        QWindowCapture::capturableWindows();
    qInfo().noquote() << "[probe] capturable windows:" << windows.size();
    for (int i = 0; i < windows.size(); ++i) {
        qInfo().noquote() << "[probe]  [" << i << "]"
                          << windows.at(i).description();
    }
}

void listQtAudioDevices()
{
    qInfo().noquote() << "[probe] Qt audio inputs:"
                      << QMediaDevices::audioInputs().size();
    for (const QAudioDevice &device : QMediaDevices::audioInputs()) {
        const QAudioFormat format = device.preferredFormat();
        qInfo().noquote() << "[probe]  input"
                          << device.description()
                          << "id=" << QString::fromUtf8(device.id())
                          << "rate=" << format.sampleRate()
                          << "channels=" << format.channelCount()
                          << "sampleFormat=" << static_cast<int>(format.sampleFormat());
    }

    qInfo().noquote() << "[probe] Qt audio outputs:"
                      << QMediaDevices::audioOutputs().size();
    for (const QAudioDevice &device : QMediaDevices::audioOutputs()) {
        qInfo().noquote() << "[probe]  output"
                          << device.description()
                          << "id=" << QString::fromUtf8(device.id());
    }

    qInfo().noquote()
        << "[probe] 说明：Qt 的 audioInputs() 不列出 PulseAudio 的 monitor source；"
           "这是预期的（见方案 1.4）。";
}

#ifdef SYNCINE_PROBE_HAVE_PULSE
struct PulseSource {
    QString name;
    QString description;
    quint32 monitorOfSink = PA_INVALID_INDEX;
    int sampleRate = 0;
    int channels = 0;
    int sampleFormat = 0;
};

void pulseSourceCallback(pa_context *, const pa_source_info *info, int eol,
                         void *userdata)
{
    if (eol != 0)
        return;

    auto *sources = static_cast<QList<PulseSource> *>(userdata);
    PulseSource source;
    source.name = QString::fromUtf8(info->name);
    source.description = QString::fromUtf8(info->description);
    source.monitorOfSink = info->monitor_of_sink;
    source.sampleRate = static_cast<int>(info->sample_spec.rate);
    source.channels = static_cast<int>(info->sample_spec.channels);
    source.sampleFormat = static_cast<int>(info->sample_spec.format);
    sources->append(source);
}

void listPulseSources()
{
    pa_mainloop *mainloop = pa_mainloop_new();
    if (mainloop == nullptr) {
        qWarning().noquote() << "[probe] libpulse: 创建 mainloop 失败";
        return;
    }

    pa_mainloop_api *api = pa_mainloop_get_api(mainloop);
    pa_context *context = pa_context_new(api, "syncine-capture-probe");
    if (context == nullptr) {
        qWarning().noquote() << "[probe] libpulse: 创建 context 失败";
        pa_mainloop_free(mainloop);
        return;
    }

    pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr);

    bool ready = false;
    for (;;) {
        const pa_context_state_t state = pa_context_get_state(context);
        if (state == PA_CONTEXT_READY) {
            ready = true;
            break;
        }
        if (!PA_CONTEXT_IS_GOOD(state)) {
            qWarning().noquote() << "[probe] libpulse: 连接失败 state="
                                 << static_cast<int>(state);
            break;
        }
        if (pa_mainloop_iterate(mainloop, 1, nullptr) < 0)
            break;
    }

    if (ready) {
        QList<PulseSource> sources;
        pa_operation *operation =
            pa_context_get_source_info_list(context, pulseSourceCallback, &sources);
        while (operation != nullptr
               && pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
            if (pa_mainloop_iterate(mainloop, 1, nullptr) < 0)
                break;
        }
        if (operation != nullptr)
            pa_operation_unref(operation);

        qInfo().noquote() << "[probe] PulseAudio sources:" << sources.size();
        for (const PulseSource &source : sources) {
            const bool monitor = source.monitorOfSink != PA_INVALID_INDEX;
            qInfo().noquote() << "[probe]  source"
                              << source.description
                              << "name=" << source.name
                              << "monitor=" << monitor
                              << "monitorOfSink=" << source.monitorOfSink
                              << "rate=" << source.sampleRate
                              << "channels=" << source.channels
                              << "format=" << source.sampleFormat;
        }
    }

    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_mainloop_free(mainloop);
}
#else
void listPulseSources()
{
    qInfo().noquote()
        << "[probe] 此构建未启用 libpulse 枚举（缺少 libpulse-dev 或非 Linux 构建）";
}
#endif

void listAudio()
{
    listQtAudioDevices();
    listPulseSources();
}

} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("syncine_capture_probe"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Syncine Phase 0 采集探针：打印屏幕/窗口采集帧的格式、"
                       "线程、行距与系统声音 monitor 枚举。"));
    parser.addHelpOption();

    QCommandLineOption listOption({QStringLiteral("l"), QStringLiteral("list")},
                                  QStringLiteral("列出屏幕、窗口与音频设备"));
    QCommandLineOption screenOption({QStringLiteral("s"), QStringLiteral("screen")},
                                    QStringLiteral("采集指定屏幕（索引，默认 0）"),
                                    QStringLiteral("index"), QStringLiteral("0"));
    QCommandLineOption windowOption({QStringLiteral("w"), QStringLiteral("window")},
                                    QStringLiteral("采集指定窗口（索引）"),
                                    QStringLiteral("index"));
    QCommandLineOption titleOption({QStringLiteral("t"), QStringLiteral("window-title")},
                                   QStringLiteral("按标题子串选择窗口"),
                                   QStringLiteral("text"));
    QCommandLineOption selfTestOption(QStringLiteral("selftest"),
                                      QStringLiteral("弹出四色块窗口并抓取它，验证颜色通道"));
    QCommandLineOption durationOption({QStringLiteral("d"), QStringLiteral("duration")},
                                      QStringLiteral("采集秒数（默认 5）"),
                                      QStringLiteral("seconds"), QStringLiteral("5"));
    QCommandLineOption saveOption(QStringLiteral("save"),
                                  QStringLiteral("保存首帧 PNG（含屏幕内容，默认不保存）"),
                                  QStringLiteral("path"));
    QCommandLineOption audioOption({QStringLiteral("a"), QStringLiteral("audio")},
                                   QStringLiteral("只列出音频设备与 Pulse monitor"));

    parser.addOptions({listOption, screenOption, windowOption, titleOption,
                       selfTestOption, durationOption, saveOption, audioOption});
    parser.process(app);

    if (parser.isSet(audioOption)) {
        listAudio();
        return 0;
    }
    if (parser.isSet(listOption)) {
        listScreensAndWindows();
        listAudio();
        return 0;
    }

    Options options;
    options.durationSec = qBound(1, parser.value(durationOption).toInt(), 60);
    options.savePath = parser.value(saveOption);
    options.windowIndex = parser.isSet(windowOption)
                              ? parser.value(windowOption).toInt()
                              : -1;
    options.windowTitle = parser.value(titleOption);

    if (parser.isSet(selfTestOption)) {
        options.mode = Options::Mode::SelfTest;
    } else if (parser.isSet(windowOption) || parser.isSet(titleOption)) {
        options.mode = Options::Mode::Window;
        if (parser.isSet(windowOption))
            options.windowIndex = parser.value(windowOption).toInt();
    } else if (parser.isSet(screenOption)) {
        options.mode = Options::Mode::Screen;
        options.screenIndex = parser.value(screenOption).toInt();
    } else {
        listScreensAndWindows();
        listAudio();
        return 0;
    }

    CaptureProbe probe(options);
    if (!probe.start())
        return 2;

    return app.exec();
}
