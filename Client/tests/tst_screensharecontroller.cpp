//
// ScreenShareController 的运行时冒烟测试（需要真实图形会话）。
//
// 它启动一次整屏采集，统计 frameAvailable 的频率与尺寸，再停止。
// 不保存任何画面，只验证：
//   · refreshSources 能枚举屏幕；
//   · startScreen 成功；
//   · 帧通过信号送出来，且被节流到配置值附近；
//   · stop 后 active=false，不再来帧。
//
// 不放进 CTest：CI 的 offscreen 平台没有屏幕可采。
// 手动运行：QT_QPA_PLATFORM=xcb ./syncine_screenshare_tests
//

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPainter>
#include <QRasterWindow>
#include <QThread>
#include <QTimer>
#include <QVideoFrame>

#include <atomic>
#include <cstdio>

#include "capture/ScreenShareController.h"
#include "capture/SystemAudioCapture.h"

namespace {

int g_failures = 0;

// 屏幕上有一点持续变化，避免 X11 的 XDamage 在完全静止时不给帧，
// 从而让“帧率是否被节流”变得可测。
class AnimatedWindow : public QRasterWindow
{
public:
    AnimatedWindow()
    {
        resize(320, 240);
        setPosition(40, 40);
        setFlag(Qt::WindowStaysOnTopHint);
        m_timer.setInterval(16);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] {
            m_phase = (m_phase + 9) % 256;
            update();
        });
        m_timer.start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(QRect(0, 0, width(), height()),
                         QColor(m_phase,
                                (m_phase * 3) % 256,
                                (m_phase * 7) % 256));
    }

private:
    QTimer m_timer;
    int m_phase = 0;
};

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    // 用 20fps 明确验证节流；屏幕本身通常 60fps 以上。
    qputenv("SYNCINE_SCREEN_FPS", "20");

    AnimatedWindow animatedWindow;
    animatedWindow.show();

    ScreenShareController controller;
    int frames = 0;
    QSize lastSize;
    QElapsedTimer elapsed;
    bool sawNonGuiThread = false;
    std::atomic<qint64> audioBytes{0};
    std::atomic<int> audioCallbacks{0};

    controller.setAudioSink(
        [&audioBytes, &audioCallbacks](const int16_t *, size_t samples,
                                       int, size_t channels) {
            audioBytes.fetch_add(static_cast<qint64>(samples * channels * 2),
                                 std::memory_order_relaxed);
            audioCallbacks.fetch_add(1, std::memory_order_relaxed);
        });
    const bool audioAvailable = SystemAudioCapture::isSupported();
    if (audioAvailable)
        controller.setAudioEnabled(true);

    QObject::connect(&controller, &ScreenShareController::activeChanged, &app,
                     [&controller] {
                         std::printf("active=%d label=%s error=%s\n",
                                     controller.isActive() ? 1 : 0,
                                     qPrintable(controller.sourceLabel()),
                                     qPrintable(controller.errorString()));
                     });
    QObject::connect(&controller, &ScreenShareController::frameAvailable, &app,
                     [&frames, &lastSize, &sawNonGuiThread](const QVideoFrame &frame) {
                         ++frames;
                         lastSize = frame.size();
                         if (QThread::currentThread()
                             != QCoreApplication::instance()->thread()) {
                             sawNonGuiThread = true;
                         }
                     });

    controller.refreshSources();
    std::printf("screens=%lld windows=%lld\n",
                static_cast<long long>(controller.screens().size()),
                static_cast<long long>(controller.windows().size()));

    if (controller.screens().isEmpty()) {
        std::fprintf(stderr, "没有可用屏幕，跳过\n");
        return 0;
    }

    elapsed.start();
    if (!controller.startScreen(0)) {
        std::fprintf(stderr, "startScreen 失败: %s\n",
                     qPrintable(controller.errorString()));
        return 1;
    }

    QTimer::singleShot(2000, &app, [&] {
        controller.stop();
        const double seconds = elapsed.elapsed() / 1000.0;
        const double fps = seconds > 0.0 ? frames / seconds : 0.0;
        std::printf("frames=%d seconds=%.2f fps=%.2f size=%dx%d\n",
                    frames, seconds, fps, lastSize.width(), lastSize.height());
        std::printf("audioAvailable=%d audioActive=%d callbacks=%d bytes=%lld error=%s\n",
                    audioAvailable ? 1 : 0, controller.audioActive() ? 1 : 0,
                    audioCallbacks.load(), static_cast<long long>(audioBytes.load()),
                    qPrintable(controller.audioError()));

        // 20fps 节流允许一定误差；采集后端本身可能低于这个值。
        CHECK(frames > 10);
        CHECK(fps > 12.0 && fps < 26.0);
        CHECK(lastSize.isValid());
        CHECK(!controller.isActive());
        CHECK(!sawNonGuiThread); // 控制器应把帧统一搬到 GUI 线程

        if (audioAvailable) {
            // 静音也应该有连续的 PCM（monitor 输出静音）；只验回调与字节数。
            CHECK(audioCallbacks.load() > 0);
            CHECK(audioBytes.load() > 0);
            CHECK(!controller.audioActive());
        }

        QCoreApplication::quit();
    });

    const int code = app.exec();
    if (g_failures != 0) {
        std::fprintf(stderr, "screenshare: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("screenshare: all checks passed\n");
    return code;
}
