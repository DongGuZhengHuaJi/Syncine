// VideoTrackSource 的 worker/FIFO/时间戳/shutdown 回归测试。
//
// 不建 PeerConnection：用假 sink 直接接收 VideoFrame，合成 CPU YUV420P 帧。

#include <QCoreApplication>
#include <QThread>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#include "webrtc/VideoTrackSource.h"

#include "libyuv/convert_argb.h"

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
        m_lastWidth = frame.width();
        m_lastHeight = frame.height();
        m_lastFrame = frame;
        ++m_count;
    }

    int count() const { return m_count; }
    bool nonIncreasing() const { return m_nonIncreasing; }
    int lastWidth() const { return m_lastWidth; }
    int lastHeight() const { return m_lastHeight; }
    const webrtc::VideoFrame &lastFrame() const { return *m_lastFrame; }

private:
    int m_count = 0;
    int m_lastWidth = 0;
    int m_lastHeight = 0;
    int64_t m_lastTimestampUs = 0;
    bool m_nonIncreasing = false;
    std::optional<webrtc::VideoFrame> m_lastFrame;
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

// 生成四象限 RGB 测试帧：左上红、右上绿、左下蓝、右下白。
// 按 Qt 枚举的内存字节序写入，确保被测的是 VideoTrackSource 的映射，
// 而不是测试代码和实现共用同一个错误假设。
QVideoFrame makeRgbFrame(QVideoFrameFormat::PixelFormat format,
                         int width, int height)
{
    QVideoFrameFormat frameFormat(QSize(width, height), format);
    QVideoFrame frame(frameFormat);
    if (!frame.map(QVideoFrame::WriteOnly))
        return {};

    auto writePixel = [&](int x, int y, int r, int g, int b) {
        uchar *row = frame.bits(0) + static_cast<ptrdiff_t>(y)
                                      * frame.bytesPerLine(0);
        uchar *p = row + static_cast<ptrdiff_t>(x) * 4;
        switch (format) {
        case QVideoFrameFormat::Format_BGRA8888:
        case QVideoFrameFormat::Format_BGRA8888_Premultiplied:
        case QVideoFrameFormat::Format_BGRX8888:
            p[0] = static_cast<uchar>(b);
            p[1] = static_cast<uchar>(g);
            p[2] = static_cast<uchar>(r);
            p[3] = 255;
            break;
        case QVideoFrameFormat::Format_RGBA8888:
        case QVideoFrameFormat::Format_RGBX8888:
            p[0] = static_cast<uchar>(r);
            p[1] = static_cast<uchar>(g);
            p[2] = static_cast<uchar>(b);
            p[3] = 255;
            break;
        case QVideoFrameFormat::Format_ARGB8888:
        case QVideoFrameFormat::Format_ARGB8888_Premultiplied:
        case QVideoFrameFormat::Format_XRGB8888:
            p[0] = 255;
            p[1] = static_cast<uchar>(r);
            p[2] = static_cast<uchar>(g);
            p[3] = static_cast<uchar>(b);
            break;
        case QVideoFrameFormat::Format_ABGR8888:
        case QVideoFrameFormat::Format_XBGR8888:
            p[0] = 255;
            p[1] = static_cast<uchar>(b);
            p[2] = static_cast<uchar>(g);
            p[3] = static_cast<uchar>(r);
            break;
        default:
            break;
        }
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool right = x >= width / 2;
            const bool bottom = y >= height / 2;
            if (!right && !bottom)
                writePixel(x, y, 255, 0, 0);
            else if (right && !bottom)
                writePixel(x, y, 0, 255, 0);
            else if (!right && bottom)
                writePixel(x, y, 0, 0, 255);
            else
                writePixel(x, y, 255, 255, 255);
        }
    }

    frame.unmap();
    return frame;
}

bool expectPixel(const std::vector<uchar> &argb, int width,
                 int x, int y, int r, int g, int b)
{
    const size_t index = (static_cast<size_t>(y) * width + x) * 4;
    // libyuv 的 I420ToARGB 输出“ARGB little endian”，内存顺序是 B,G,R,A。
    const int actualB = argb[index];
    const int actualG = argb[index + 1];
    const int actualR = argb[index + 2];
    const bool ok = std::abs(actualR - r) <= 12
                    && std::abs(actualG - g) <= 12
                    && std::abs(actualB - b) <= 12;
    if (!ok) {
        std::fprintf(stderr,
                     "  pixel(%d,%d): expected rgb(%d,%d,%d), actual rgb(%d,%d,%d)\n",
                     x, y, r, g, b, actualR, actualG, actualB);
    }
    return ok;
}

bool frameHasExpectedQuadrants(const webrtc::VideoFrame &frame,
                               int width, int height)
{
    const webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
        frame.video_frame_buffer()->ToI420();
    if (i420 == nullptr)
        return false;

    std::vector<uchar> argb(static_cast<size_t>(width) * height * 4);
    libyuv::I420ToARGB(
        i420->DataY(), i420->StrideY(),
        i420->DataU(), i420->StrideU(),
        i420->DataV(), i420->StrideV(),
        argb.data(), width * 4, width, height);

    return expectPixel(argb, width, width / 4, height / 4, 255, 0, 0)
        && expectPixel(argb, width, width * 3 / 4, height / 4, 0, 255, 0)
        && expectPixel(argb, width, width / 4, height * 3 / 4, 0, 0, 255)
        && expectPixel(argb, width, width * 3 / 4, height * 3 / 4, 255, 255, 255);
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

        // 编码前缩放：64x48 的短边 48 → 24，输出 32x24。
        source.setMaxEncodeHeight(24);
        source.pushQtFrame(makeYuvFrame(64, 48));
        CHECK(waitFor(sink, kFrames + 1));
        CHECK(sink.lastWidth() == 32);
        CHECK(sink.lastHeight() == 24);

        source.RemoveSink(&sink);
        CHECK(source.sinkCount() == 0);
        source.shutdown();
    }

    {
        // RGB 源：屏幕/窗口采集在 X11 给 BGRX8888，Windows 常见 BGRA8888。
        // 每种格式都用同一个四象限图案，检查转换后的颜色与尺寸。
        VideoTrackSource source;
        FakeSink sink;
        source.AddOrUpdateSink(&sink, webrtc::VideoSinkWants{});

        const QVideoFrameFormat::PixelFormat formats[] = {
            QVideoFrameFormat::Format_BGRA8888,
            QVideoFrameFormat::Format_BGRX8888,
            QVideoFrameFormat::Format_RGBA8888,
            QVideoFrameFormat::Format_RGBX8888,
            QVideoFrameFormat::Format_ARGB8888,
            QVideoFrameFormat::Format_ABGR8888,
        };

        int expectedFrames = 0;
        for (const QVideoFrameFormat::PixelFormat format : formats) {
            const QVideoFrame frame = makeRgbFrame(format, 16, 16);
            CHECK(frame.isValid());
            source.pushQtFrame(frame);
            ++expectedFrames;
            CHECK(waitFor(sink, expectedFrames));
            CHECK(frameHasExpectedQuadrants(sink.lastFrame(), 16, 16));
        }

        // RGB 也要走编码前缩放：短边 16 → 8。
        source.setMaxEncodeHeight(8);
        source.pushQtFrame(makeRgbFrame(QVideoFrameFormat::Format_BGRX8888, 16, 16));
        ++expectedFrames;
        CHECK(waitFor(sink, expectedFrames));
        CHECK(sink.lastWidth() == 8);
        CHECK(sink.lastHeight() == 8);

        // screencast 标志会传给编码器，错误地恒为 false 会按摄像头调参。
        source.setScreenCast(true);
        CHECK(source.is_screencast());
        source.setScreenCast(false);
        CHECK(!source.is_screencast());

        source.RemoveSink(&sink);
        source.shutdown();
    }

    if (g_failures == 0) {
        std::printf("videosource: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "videosource: %d check(s) failed\n", g_failures);
    return 1;
}
