// VideoTrackSource 的 worker/FIFO/时间戳/shutdown 回归测试。
//
// 不建 PeerConnection：用假 sink 直接接收 VideoFrame，合成 CPU YUV420P 帧。

#include <QCoreApplication>
#include <QThread>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "webrtc/VideoTrackSource.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

class FakeSink : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    void OnFrame(const webrtc::VideoFrame &frame) override {
        const int64_t ts = frame.timestamp_us();
        if (m_count > 0 && ts <= m_lastTimestampUs)
            m_nonIncreasing = true;
        m_lastTimestampUs = ts;
        ++m_count;
    }

    int count() const { return m_count; }
    bool nonIncreasing() const { return m_nonIncreasing; }

private:
    int m_count = 0;
    int64_t m_lastTimestampUs = 0;
    bool m_nonIncreasing = false;
};

QVideoFrame makeYuvFrame(int width, int height)
{
    QVideoFrameFormat format(QSize(width, height),
                             QVideoFrameFormat::Format_YUV420P);
    QVideoFrame frame(format);
    if (!frame.map(QVideoFrame::WriteOnly))
        return {};

    for (int plane = 0; plane < 3; ++plane) {
        uchar *bits = frame.bits(plane);
        const int stride = frame.bytesPerLine(plane);
        const int rows = plane == 0 ? height : (height + 1) / 2;
        const int cols = plane == 0 ? width : (width + 1) / 2;
        for (int row = 0; row < rows; ++row)
            std::memset(bits + static_cast<ptrdiff_t>(row) * stride,
                        plane == 0 ? 16 : 128, static_cast<size_t>(cols));
    }
    frame.unmap();
    return frame;
}

bool waitFor(const FakeSink &sink, int expected, int timeoutMs = 3000)
{
    const int step = 10;
    for (int waited = 0; waited < timeoutMs; waited += step) {
        if (sink.count() >= expected)
            return true;
        QThread::msleep(step);
    }
    return sink.count() >= expected;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    {
        VideoTrackSource source;

        // 没有订阅者时不转换、不广播。
        source.pushQtFrame(makeYuvFrame(64, 48));
        QThread::msleep(50);

        FakeSink sink;
        source.AddOrUpdateSink(&sink, webrtc::VideoSinkWants{});
        CHECK(source.sinkCount() == 1);

        constexpr int kFrames = 10;
        for (int i = 0; i < kFrames; ++i) {
            source.pushQtFrame(makeYuvFrame(64, 48));
            QThread::msleep(5);
        }

        CHECK(waitFor(sink, kFrames));
        CHECK(sink.count() == kFrames);
        CHECK(!sink.nonIncreasing());

        source.RemoveSink(&sink);
        CHECK(source.sinkCount() == 0);
        source.shutdown();
    }

    if (g_failures == 0) {
        std::printf("videosource: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "videosource: %d check(s) failed\n", g_failures);
    return 1;
}
