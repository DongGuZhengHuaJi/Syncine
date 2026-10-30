//
// Created by donggu on 2026/10/30.
//

#include "ScreenShareController.h"

#include <QFile>
#include <QGuiApplication>
#include <QMetaObject>
#include <QRegularExpression>
#include <QScreen>
#include <QStringList>
#include <QThread>
#include <QVariantMap>
#include <QtGlobal>

#include "core/Log.h"

#if defined(Q_OS_LINUX)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif

namespace {

QString screenLabel(int index, const QScreen *screen)
{
    const QRect geometry = screen->geometry();
    const QString name = screen->name().isEmpty()
                             ? QStringLiteral("屏幕 %1").arg(index + 1)
                             : QStringLiteral("屏幕 %1 · %2").arg(index + 1)
                                   .arg(screen->name());
    return QStringLiteral("%1（%2×%3）")
        .arg(name)
        .arg(geometry.width())
        .arg(geometry.height());
}

#if defined(Q_OS_LINUX)
// Xlib 的默认错误处理器会把 BadWindow 当成致命错误直接退出进程。
// 窗口可能在枚举后立刻关闭，查询属性时必须临时吞掉这类错误。
int ignoreX11Error(Display *, XErrorEvent *)
{
    return 0;
}

// X11 下 Qt 对没有 _NET_WM_NAME/WM_NAME 的窗口会给出 "Window 0x..." 这样的
// 描述。它其实是 X window id：可以拿来查 WM_CLASS / _NET_WM_PID，
// 给用户一个可辨认的应用名，而不是一串地址。
QString x11AppNameForWindow(Display *display, const QString &description)
{
    static const QRegularExpression addressPattern(
        QStringLiteral("^Window 0x([0-9a-fA-F]+)$"));
    const QRegularExpressionMatch match = addressPattern.match(description);
    if (!match.hasMatch())
        return {};

    bool ok = false;
    const unsigned long rawId = match.captured(1).toULongLong(&ok, 16);
    if (!ok)
        return {};

    const ::Window window = static_cast<::Window>(rawId);
    QString appName;

    // 串行化并在查询期间替换 Xlib 错误处理器。其他线程若同时使用 Xlib，
    // 其错误也会被短暂忽略；窗口枚举是低频操作，代价可接受。
    static QMutex errorHandlerMutex;
    QMutexLocker errorHandlerLock(&errorHandlerMutex);
    XErrorHandler previousHandler = XSetErrorHandler(ignoreX11Error);

    // WM_CLASS 通常是 "instance\0Class\0"，取最后一个非空段（Class）。
    Atom type = None;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char *data = nullptr;
    const Atom wmClassAtom = XInternAtom(display, "WM_CLASS", False);
    if (XGetWindowProperty(display, window, wmClassAtom, 0, 1024, False,
                           XA_STRING, &type, &format, &count, &remaining,
                           &data) == Success
        && data != nullptr) {
        const QList<QByteArray> parts =
            QByteArray(reinterpret_cast<const char *>(data),
                       static_cast<int>(count)).split('\0');
        XFree(data);
        for (const QByteArray &part : parts) {
            if (!part.trimmed().isEmpty())
                appName = QString::fromLocal8Bit(part.trimmed());
        }
    }

    // WM_CLASS 为空时用进程名兜底。
    if (appName.isEmpty()) {
        data = nullptr;
        const Atom pidAtom = XInternAtom(display, "_NET_WM_PID", False);
        if (XGetWindowProperty(display, window, pidAtom, 0, 1, False,
                               XA_CARDINAL, &type, &format, &count, &remaining,
                               &data) == Success
            && data != nullptr) {
            if (count >= 1) {
                const unsigned long pid =
                    *reinterpret_cast<const unsigned long *>(data);
                QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
                if (comm.open(QIODevice::ReadOnly)) {
                    appName = QString::fromLocal8Bit(comm.readAll().trimmed());
                }
            }
            XFree(data);
        }
    }

    XSetErrorHandler(previousHandler);
    return appName;
}
#endif

QString windowLabel(void *display, int index, const QCapturableWindow &window)
{
#if !defined(Q_OS_LINUX)
    Q_UNUSED(display);
#endif
    const QString description = window.description();
    QString label;

    const bool empty = description.trimmed().isEmpty();
    // Qt X11 后端的占位描述形如 "Window 0x1800011"。
    const bool addressLike =
        description.startsWith(QLatin1String("Window 0x"), Qt::CaseInsensitive);

    if (empty || addressLike) {
#if defined(Q_OS_LINUX)
        QString appName;
        if (display != nullptr) {
            appName = x11AppNameForWindow(static_cast<Display *>(display),
                                          description);
        }
        if (!appName.isEmpty()) {
            label = QStringLiteral("%1 · 窗口 %2").arg(appName).arg(index + 1);
        } else
#endif
        {
            label = QStringLiteral("窗口 %1（无标题）").arg(index + 1);
        }
    } else {
        label = description;
    }

    if (!window.isValid())
        label += QStringLiteral("（已关闭）");
    return label;
}

} // namespace

ScreenShareController::ScreenShareController(QObject *parent)
    : QObject(parent)
{
    int fps = 30;
    if (qEnvironmentVariableIsSet("SYNCINE_SCREEN_FPS")) {
        bool ok = false;
        const int configured = qEnvironmentVariableIntValue("SYNCINE_SCREEN_FPS", &ok);
        if (ok)
            fps = configured;
    }
    fps = qBound(1, fps, 60);
    m_minFrameIntervalMs = qMax(1, 1000 / fps);

    // 排查开关：SYNCINE_SCREEN_AUDIO=0 时把系统声音入口整体关掉。
    m_audioAvailable = SystemAudioCapture::isSupported();
    if (qEnvironmentVariableIsSet("SYNCINE_SCREEN_AUDIO")
        && qEnvironmentVariableIntValue("SYNCINE_SCREEN_AUDIO") == 0) {
        m_audioAvailable = false;
    }

    connect(&m_videoSink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) { onCapturedFrame(frame); });

    connect(&m_screenCapture, &QScreenCapture::errorOccurred, this,
            [this](QScreenCapture::Error error, const QString &text) {
                onError(QStringLiteral("屏幕采集失败（%1）：%2")
                            .arg(static_cast<int>(error)).arg(text));
            });
    connect(&m_windowCapture, &QWindowCapture::errorOccurred, this,
            [this](QWindowCapture::Error error, const QString &text) {
                onError(QStringLiteral("窗口采集失败（%1）：%2")
                            .arg(static_cast<int>(error)).arg(text));
            });
}

ScreenShareController::~ScreenShareController()
{
    stop();
}

bool ScreenShareController::audioAvailable() const
{
    return m_audioAvailable;
}

void ScreenShareController::setAudioEnabled(bool enabled)
{
    if (!m_audioAvailable)
        enabled = false;
    if (m_audioEnabled == enabled)
        return;

    m_audioEnabled = enabled;
    emit audioEnabledChanged();

    if (!isActive())
        return;

    if (m_audioEnabled)
        startAudioCapture();
    else
        stopAudioCapture();
}

void ScreenShareController::setOutputDeviceName(const QString &name)
{
    if (m_outputDeviceName == name)
        return;

    const bool restart = audioActive();
    stopAudioCapture();
    m_outputDeviceName = name;
    if (restart && m_audioEnabled && isActive())
        startAudioCapture();
}

void ScreenShareController::startAudioCapture()
{
    if (!m_audioAvailable || !m_audioEnabled || !isActive())
        return;
    if (audioActive())
        return;
    if (!m_audioSink) {
        setAudioError(QStringLiteral("系统声音接收端未装配"));
        return;
    }

    stopAudioCapture();
    m_audioCapture = SystemAudioCapture::create();
    if (!m_audioCapture) {
        setAudioError(QStringLiteral("当前构建不支持系统声音采集"));
        return;
    }

    if (!m_audioError.isEmpty()) {
        m_audioError.clear();
        emit audioErrorChanged();
    }

    const bool started = m_audioCapture->start(
        m_outputDeviceName,
        [this](const int16_t *data, size_t samples, int rate, size_t channels) {
            onAudioPcm(data, samples, rate, channels);
        },
        [this](const QString &message) { onAudioError(message); });

    if (!started) {
        m_audioCapture.reset();
        return;
    }

    m_audioActive.store(true, std::memory_order_relaxed);
    LOG_INFO("ScreenAudio") << "系统声音采集已启动";
    emit audioActiveChanged();
}

void ScreenShareController::stopAudioCapture()
{
    if (m_audioCapture) {
        m_audioCapture->stop();
        m_audioCapture.reset();
    }

    if (m_audioActive.exchange(false, std::memory_order_relaxed)) {
        LOG_INFO("ScreenAudio") << "系统声音采集已停止";
        emit audioActiveChanged();
    }
}

void ScreenShareController::onAudioPcm(const int16_t *data,
                                       size_t samplesPerChannel,
                                       int sampleRate,
                                       size_t channels)
{
    if (!m_audioActive.load(std::memory_order_relaxed))
        return;
    if (m_audioSink)
        m_audioSink(data, samplesPerChannel, sampleRate, channels);
}

void ScreenShareController::setAudioError(const QString &message)
{
    if (m_audioError == message)
        return;
    m_audioError = message;
    LOG_WARN("ScreenAudio") << message;
    emit audioErrorChanged();
}

void ScreenShareController::onAudioError(const QString &message)
{
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, message] { onAudioError(message); },
                                  Qt::QueuedConnection);
        return;
    }

    setAudioError(message);
    stopAudioCapture();
}

void ScreenShareController::refreshSources()
{
    m_screens = QGuiApplication::screens();
    m_screenOptions.clear();
    for (int i = 0; i < m_screens.size(); ++i) {
        QVariantMap option;
        option.insert(QStringLiteral("label"), screenLabel(i, m_screens.at(i)));
        option.insert(QStringLiteral("value"), i);
        m_screenOptions.append(option);
    }

    m_windows = QWindowCapture::capturableWindows();

#if defined(Q_OS_LINUX)
    // 只用于把 "Window 0x..." 解析成应用名；打不开时仍能列出窗口。
    Display *display = XOpenDisplay(nullptr);
#else
    void *display = nullptr;
#endif

    m_windowOptions.clear();
    for (int i = 0; i < m_windows.size(); ++i) {
        const QCapturableWindow &window = m_windows.at(i);

        QVariantMap option;
        option.insert(QStringLiteral("label"),
                      windowLabel(display, i, window));
        option.insert(QStringLiteral("value"), i);
        m_windowOptions.append(option);
    }

#if defined(Q_OS_LINUX)
    if (display != nullptr)
        XCloseDisplay(display);
#endif

    LOG_DEBUG("ScreenShare") << "采集源刷新: 屏幕" << m_screenOptions.size()
                             << "窗口" << m_windowOptions.size();
    emit sourcesChanged();
}

bool ScreenShareController::startScreen(int index)
{
    stop();

    if (index < 0 || index >= m_screens.size()) {
        setError(QStringLiteral("屏幕索引无效"));
        return false;
    }

    QScreen *screen = m_screens.at(index);
    if (screen == nullptr) {
        setError(QStringLiteral("屏幕已不存在"));
        return false;
    }

    if (!m_errorString.isEmpty()) {
        m_errorString.clear();
        emit errorChanged();
    }

    m_sourceKind = QStringLiteral("screen");
    m_sourceLabel = screenLabel(index, screen);
    LOG_INFO("ScreenShare") << "开始共享" << m_sourceLabel;

    m_session.setWindowCapture(nullptr);
    m_session.setScreenCapture(&m_screenCapture);
    m_screenCapture.setScreen(screen);
    m_session.setVideoSink(&m_videoSink);

    resetFrameState();
    m_active.store(true, std::memory_order_relaxed);
    m_screenCapture.start();

    if (m_screenCapture.error() != QScreenCapture::NoError) {
        setError(QStringLiteral("屏幕采集启动失败：%1")
                     .arg(m_screenCapture.errorString()));
        stop();
        return false;
    }

    emit activeChanged();
    startAudioCapture();
    return true;
}

bool ScreenShareController::startWindow(int index)
{
    stop();

    if (index < 0 || index >= m_windows.size()) {
        setError(QStringLiteral("窗口索引无效"));
        return false;
    }

    const QCapturableWindow window = m_windows.at(index);
    if (!window.isValid()) {
        setError(QStringLiteral("窗口已关闭"));
        return false;
    }

    if (!m_errorString.isEmpty()) {
        m_errorString.clear();
        emit errorChanged();
    }

    m_sourceKind = QStringLiteral("window");
    m_sourceLabel = window.description().isEmpty()
                        ? QStringLiteral("窗口 %1").arg(index + 1)
                        : window.description();
    LOG_INFO("ScreenShare") << "开始共享窗口:" << m_sourceLabel;

    m_session.setScreenCapture(nullptr);
    m_session.setWindowCapture(&m_windowCapture);
    m_windowCapture.setWindow(window);
    m_session.setVideoSink(&m_videoSink);

    resetFrameState();
    m_active.store(true, std::memory_order_relaxed);
    m_windowCapture.start();

    if (m_windowCapture.error() != QWindowCapture::NoError) {
        setError(QStringLiteral("窗口采集启动失败：%1")
                     .arg(m_windowCapture.errorString()));
        stop();
        return false;
    }

    emit activeChanged();
    startAudioCapture();
    return true;
}

void ScreenShareController::stop()
{
    const bool wasActive = m_active.exchange(false, std::memory_order_relaxed);

    stopAudioCapture();
    m_screenCapture.stop();
    m_windowCapture.stop();

    // session 只持有裸指针：先清掉，再让它背后的对象析构。
    m_session.setScreenCapture(nullptr);
    m_session.setWindowCapture(nullptr);
    m_session.setVideoSink(nullptr);

    {
        QMutexLocker locker(&m_frameMutex);
        m_latestFrame = {};
        m_dispatchPending = false;
        m_lastAcceptedMs = -1;
    }

    if (wasActive) {
        LOG_INFO("ScreenShare") << "停止共享" << m_sourceLabel;
        m_sourceLabel.clear();
        m_sourceKind = QStringLiteral("media");
        emit activeChanged();
    }
}

void ScreenShareController::resetFrameState()
{
    QMutexLocker locker(&m_frameMutex);
    m_latestFrame = {};
    m_dispatchPending = false;
    m_clockStart = std::chrono::steady_clock::now();
    m_lastAcceptedMs = -1;
}

void ScreenShareController::onCapturedFrame(const QVideoFrame &frame)
{
    if (!m_active.load(std::memory_order_relaxed) || !frame.isValid())
        return;

    bool direct = false;
    {
        QMutexLocker locker(&m_frameMutex);

        if (!m_active.load(std::memory_order_relaxed))
            return;

        // 帧率节流放在最前面：屏幕采集通常是 60/120fps，这里默认只要 30fps。
        // 先丢帧再谈转换，避免逐帧搬运把 GUI/CPU 拖满。
        //
        // 留一点容差：采集帧间隔往往略小于 1000/fps（例如 62fps 约 16.1ms），
        // 若严格按 33ms 判断，帧落在 16/32/48ms 时会连续丢掉两帧，
        // 实际只剩 20fps 左右。减去一个小的容差后，32ms 那一帧才会被接受。
        const qint64 toleranceMs = qMax<qint64>(1, m_minFrameIntervalMs / 6);
        const auto now = std::chrono::steady_clock::now();
        const qint64 nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - m_clockStart).count();
        if (m_lastAcceptedMs >= 0
            && nowMs - m_lastAcceptedMs < m_minFrameIntervalMs - toleranceMs) {
            return;
        }
        m_lastAcceptedMs = nowMs;

        if (QThread::currentThread() != thread()) {
            // 非 GUI 线程：只保留最新一帧，已有的待处理投递不再追加。
            m_latestFrame = frame;
            if (m_dispatchPending)
                return;
            m_dispatchPending = true;

            QMetaObject::invokeMethod(this, [this] {
                QVideoFrame latest;
                {
                    QMutexLocker locker(&m_frameMutex);
                    latest = m_latestFrame;
                    m_latestFrame = {};
                    m_dispatchPending = false;
                }
                if (m_active.load(std::memory_order_relaxed) && latest.isValid())
                    emit frameAvailable(latest);
            }, Qt::QueuedConnection);
            return;
        }

        direct = true;
    }

    if (direct && m_active.load(std::memory_order_relaxed))
        emit frameAvailable(frame);
}

void ScreenShareController::setError(const QString &message)
{
    if (m_errorString == message)
        return;

    m_errorString = message;
    LOG_WARN("ScreenShare") << message;
    emit errorChanged();
    emit errorOccurred(message);
}

void ScreenShareController::onError(const QString &message)
{
    // 采集后端的 errorOccurred 可能不在 GUI 线程；stop() 必须在 GUI 线程。
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, [this, message] { onError(message); },
                                  Qt::QueuedConnection);
        return;
    }

    setError(message);
    stop();
}
