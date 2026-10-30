//
// 房主端屏幕共享预览：采集帧要出现在界面的 QVideoSink 上，
// 但不能经由 onVideoFrame 再回灌进电影推流入口。
//
// 不建 PeerConnection，也不解码媒体；PlaybackController 用真实实现，
// WebrtcManager 只补两个空实现（见 stub_webrtcmanager.cpp）。
//

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <cstddef>
#include <cstdio>

#include "playback/PlaybackController.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

QVideoFrame makeFrame(int width, int height)
{
    QVideoFrameFormat format(QSize(width, height),
                             QVideoFrameFormat::Format_BGRX8888);
    QVideoFrame frame(format);
    if (!frame.map(QVideoFrame::WriteOnly))
        return {};

    for (int y = 0; y < height; ++y) {
        uchar *row = frame.bits(0) + static_cast<ptrdiff_t>(y)
                                      * frame.bytesPerLine(0);
        for (int x = 0; x < width; ++x) {
            uchar *pixel = row + static_cast<ptrdiff_t>(x) * 4;
            pixel[0] = 0;
            pixel[1] = 0;
            pixel[2] = 255;
            pixel[3] = 255;
        }
    }
    frame.unmap();
    return frame;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    PlaybackController controller;
    QVideoSink sink;
    controller.bindVideoOutput(&sink);

    int forwarded = 0;
    QObject::connect(&controller, &PlaybackController::videoFrameAvailable,
                     &app, [&forwarded](const QVideoFrame &) { ++forwarded; });

    controller.setScreenSharePreviewActive(true);

    const QVideoFrame preview = makeFrame(320, 240);
    CHECK(preview.isValid());
    controller.presentScreenShareFrame(preview);

    const QVideoFrame shown = sink.videoFrame();
    CHECK(shown.isValid());
    CHECK(shown.width() == 320);
    CHECK(shown.height() == 240);
    // 预览帧不能从 onVideoFrame 再发一次（那会从电影入口重复推流）。
    CHECK(forwarded == 0);

    controller.setScreenSharePreviewActive(false);
    const QVideoFrame cleared = sink.videoFrame();
    CHECK(cleared.isValid());
    CHECK(cleared.width() == 2 && cleared.height() == 2);
    CHECK(forwarded == 0);

    // 可选的回归：真实媒体加载后，预览开关不能破坏 seekable/seek。
    // 需要一个真实视频路径：SYNCINE_TEST_VIDEO=/path/to/video.mp4
    const QString videoPath = qEnvironmentVariable("SYNCINE_TEST_VIDEO");
    if (!videoPath.isEmpty()) {
        controller.load(QUrl::fromLocalFile(videoPath));
        QElapsedTimer timer;
        timer.start();
        while (!controller.seekable() && timer.elapsed() < 5000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        CHECK(controller.seekable());

        controller.setScreenSharePreviewActive(true);
        controller.setScreenSharePreviewActive(false);
        CHECK(controller.seekable());

        controller.seek(500);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        CHECK(controller.position() >= 400);
    }

    controller.unbindVideoOutput();

    if (g_failures == 0) {
        std::printf("sharepreview: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "sharepreview: %d check(s) failed\n", g_failures);
    return 1;
}
