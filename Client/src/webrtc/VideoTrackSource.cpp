//
// Created by donggu on 2026/9/16.
//

#include "VideoTrackSource.h"

#include <QDebug>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include "core/Log.h"

#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include "libyuv/convert.h"
#include "libyuv/video_common.h"
#include "rtc_base/time_utils.h"

VideoTrackSource::VideoTrackSource() = default;
VideoTrackSource::~VideoTrackSource() = default;

// ============================
// Qt 帧 → I420 的格式映射
// ============================

namespace {

// 把 Qt 的像素格式翻译成 libyuv 的 FourCC
// 返回 0 表示不支持
uint32_t toLibyuvFourcc(QVideoFrameFormat::PixelFormat format) {
    switch (format) {
    case QVideoFrameFormat::Format_YUV420P:
        // Qt 的三平面 Y/U/V,就是标准 I420
        return libyuv::FOURCC_I420;
    case QVideoFrameFormat::Format_NV12:
        // Y 平面 + UV 交错平面
        return libyuv::FOURCC_NV12;
    case QVideoFrameFormat::Format_NV21:
        // Y 平面 + VU 交错平面（和 NV12 的 UV 顺序相反）
        return libyuv::FOURCC_NV21;
    case QVideoFrameFormat::Format_YUYV:
        return libyuv::FOURCC_YUY2;
    default:
        return 0;
    }
}

// 10bit 的两种布局。其 FourCC 为 0（并非「不认识」，而是没有对应的 8bit 四字码），
// 故单独判断，不可与「不支持」混为一谈。
// 必须处理的原因：HDR10 即 10bit（hevc 10bit + PQ + BT.2020），而 Qt FFmpeg 后端
// 对 10bit 内容返回的正是这两种格式；若不处理，共享模式下房主播放 HDR 时，
// 推流侧每一帧都会被丢弃，观众端看到黑屏。
bool isTenBit(QVideoFrameFormat::PixelFormat format) {
    return format == QVideoFrameFormat::Format_P010          // 半平面(实测:Qt 给的就是它)
        || format == QVideoFrameFormat::Format_YUV420P10;    // 三平面
}

} // namespace

// ============================
// 喂帧
// ============================

void VideoTrackSource::pushFrame(const webrtc::VideoFrame &frame) {
    if (frame.video_frame_buffer() == nullptr)
        return;

    m_width = frame.width();
    m_height = frame.height();
    m_hasReceivedFrame = true;

    // 广播给所有订阅者（编码器） —— 他们会各自去编码，然后发 RTP
    m_broadcaster.OnFrame(frame);
}

void VideoTrackSource::pushQtFrame(const QVideoFrame &frame) {
    if (!frame.isValid())
        return;

    // map() 不是 const 方法，而且必须和 unmap() 配对。
    // 拷一份出来（隐式共享，很廉价）,并保证所有出口都会 unmap。
    QVideoFrame mapped = frame;

    const QVideoFrameFormat::PixelFormat pixelFormat = mapped.surfaceFormat().pixelFormat();
    const uint32_t fourcc = toLibyuvFourcc(pixelFormat);
    const bool tenBit = isTenBit(pixelFormat);

    if (fourcc == 0 && !tenBit) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_ERROR("VideoSource") << "不支持的像素格式"
                                     << static_cast<int>(pixelFormat)
                                     << "—— 这一路视频不会发送。需要为它补一条到 libyuv 的映射。";
        }
        return;
    }

    if (!mapped.map(QVideoFrame::ReadOnly))
        return;

    const int width = mapped.width();
    const int height = mapped.height();

    if (width <= 0 || height <= 0) {
        mapped.unmap();
        return;
    }

    // 诊断：记录源帧的真实像素格式与行距。
    // 画面「斜切/撕裂」是行距算错的典型症状，颜色错位则是平面偏移算错的症状，
    // 两者均不报错，只能依靠这几个数值定位。Qt 6.5 起媒体后端由 GStreamer
    // 改为 FFmpeg，同一文件给出的像素格式可能与以往不同（见 CMakePresets）。
    static int diagCount = 0;
    if (diagCount < 3 || diagCount % 300 == 0) {
        LOG_TRACE("VideoSource") << "源帧: pixelFormat=" << mapped.surfaceFormat().pixelFormat()
                 << " handleType=" << mapped.handleType()
                 << " 平面数=" << mapped.planeCount()
                 << " 尺寸=" << width << "x" << height
                 << " strideY=" << mapped.bytesPerLine(0)
                 << " strideU=" << (mapped.planeCount() > 1 ? mapped.bytesPerLine(1) : 0)
                 << " strideV=" << (mapped.planeCount() > 2 ? mapped.bytesPerLine(2) : 0)
                 << " fourcc=" << fourcc;

        // 决定性的一组数字：UV 平面的实际偏移 vs libyuv 会去推算的偏移。
        // 两者不等，就说明"按显示高度推算 UV 位置"必然读错 —— 这正是上面
        // 改用 NV12ToI420(显式传指针)的原因。
        if (mapped.planeCount() > 1 && mapped.bits(0) != nullptr && mapped.bits(1) != nullptr) {
            const long long actualUvOffset = mapped.bits(1) - mapped.bits(0);
            const long long assumedUvOffset =
                static_cast<long long>(mapped.bytesPerLine(0)) * height;
            LOG_TRACE("VideoSource") << "UV 偏移: 实际=" << actualUvOffset
                     << " libyuv会推算成=" << assumedUvOffset
                     << (actualUvOffset == assumedUvOffset ? "(一致)" : "(!! 不一致,会读错 !!)");
        }
    }
    ++diagCount;

    // WebRTC 要的是**紧凑**的 I420:每个平面的行距等于宽度。
    // 而 Qt 给的帧行距往往更大（内存对齐填充）,所以不能直接把 Qt 的
    // 缓冲区交给 WebRTC —— 必须按行搬一遍，把填充去掉。
    //
    // 行距传错的症状是画面逐行错位（斜切）,而且不会有任何报错。
    const int chromaWidth = (width + 1) / 2;

    webrtc::scoped_refptr<webrtc::I420Buffer> buffer =
        webrtc::I420Buffer::Create(width, height);

    const uint8_t *srcY = mapped.bits(0);
    const int srcStrideY = mapped.bytesPerLine(0);
    const uint8_t *srcU = mapped.planeCount() > 1 ? mapped.bits(1) : nullptr;
    const int srcStrideU = mapped.planeCount() > 1 ? mapped.bytesPerLine(1) : 0;
    const uint8_t *srcV = mapped.planeCount() > 2 ? mapped.bits(2) : nullptr;
    const int srcStrideV = mapped.planeCount() > 2 ? mapped.bytesPerLine(2) : 0;

    int ret = 0;

    if (pixelFormat == QVideoFrameFormat::Format_P010) {
        // 10bit 半平面（P010），HDR 内容走这条。libyuv 无 P010→I420 的一步转换，
        // 故先降为 8bit 的 NV12 再复用下方路径。此步只降位深、不做色调映射，
        // 观众看到的是「HDR 当 SDR 用」（偏暗发灰），非本机的过曝。
        // **步进单位为元素而非字节**：libyuv 的 16bit 函数将 stride 直接用于
        // uint16_t* 指针运算，故须除以 2；传字节数会导致画面逐行错位且无任何报错。
        auto nv12 = webrtc::NV12Buffer::Create(width, height);
        ret = libyuv::P010ToNV12(
            reinterpret_cast<const uint16_t *>(srcY), srcStrideY / 2,
            reinterpret_cast<const uint16_t *>(srcU), srcStrideU / 2,
            nv12->MutableDataY(), nv12->StrideY(),
            nv12->MutableDataUV(), nv12->StrideUV(),
            width, height);

        if (ret == 0) {
            ret = libyuv::NV12ToI420(
                nv12->MutableDataY(), nv12->StrideY(),
                nv12->MutableDataUV(), nv12->StrideUV(),
                buffer->MutableDataY(), buffer->StrideY(),
                buffer->MutableDataU(), buffer->StrideU(),
                buffer->MutableDataV(), buffer->StrideV(),
                width, height);
        }
    } else if (pixelFormat == QVideoFrameFormat::Format_YUV420P10) {
        // 10bit 三平面（I010）：三块 16bit 平面，同样为元素步进。
        // 此处不涉及上方 NV12 的「显式传 UV 指针」问题——三平面各给各的指针，
        // libyuv 无需推算偏移。
        ret = libyuv::I010ToI420(
            reinterpret_cast<const uint16_t *>(srcY), srcStrideY / 2,
            reinterpret_cast<const uint16_t *>(srcU), srcStrideU / 2,
            reinterpret_cast<const uint16_t *>(srcV), srcStrideV / 2,
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            width, height);
    } else if (fourcc == libyuv::FOURCC_I420) {
        // 源已是 I420（三平面分离），行距可能大于宽度（存在填充）。
        // **必须用 I420Copy，不可用 ConvertToI420**：后者在 FOURCC_I420 模式下
        // 将三平面视为紧邻连续存放并自行计算平面偏移，从而忽略传入的行距、
        // 把填充字节当作内容读取；症状为第 0 行正确、第 1 行起全错。
        ret = libyuv::I420Copy(
            srcY, srcStrideY,
            srcU, srcStrideU,
            srcV, srcStrideV,
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            width, height);
    } else if (fourcc == libyuv::FOURCC_NV12) {
        // NV12：一个 Y 平面 + 一个 UV 交错平面。
        // **必须显式传 UV 指针，不可用 ConvertToI420**：后者按
        // src_uv = src_y + src_stride_y * src_height 自行推算 UV 位置，
        // 该式仅在解码缓冲行数等于显示高度时成立。而 H.264 宏块为 16×16，
        // 解码器按向上取整到 16 的倍数分配缓冲；显示尺寸非 16 倍数时
        // （实测 1916×1036，缓冲为 1920×1040）UV 实际位置前移，
        // 推算结果落在 Y 数据中，表现为色块错位甚至整幅斜切。
        // NV12ToI420 直接接收 UV 指针、不做任何假设，两种情况均正确。
        ret = libyuv::NV12ToI420(
            srcY, srcStrideY,
            srcU, srcStrideU,           // ← 用 Qt 给的实际 UV 指针,而不是推算出来的
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            width, height);
    } else if (fourcc == libyuv::FOURCC_NV21) {
        // NV21 与 NV12 只差 UV 两个分量的先后顺序，同样的道理。
        ret = libyuv::NV21ToI420(
            srcY, srcStrideY,
            srcU, srcStrideU,
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            width, height);
    } else {
        // YUY2 是单平面打包格式，没有第二个平面可传，不存在上面那个问题。
        ret = libyuv::YUY2ToI420(
            srcY, srcStrideY,
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            width, height);
    }

    const int rotationAngle = mapped.rotationAngle();

    // 时间戳必须使用单调时钟，不可用媒体的 startTime：
    // startTime 是媒体内时间，换源/重播时从 0 重新开始、往回 seek 时跳回更早的值，
    // 而 RTP 时间轴要求单调递增。时间戳回退会导致观众端解码器将新帧当作旧帧丢弃，
    // 直到时间轴追上（表现为卡住）；重播或换源时从 120s 跳回 0 则永远追不上，
    // 表现为永久卡死。本应用的画面同步依赖控制消息（play/pause/seek），
    // 不依赖 RTP 时间戳，故改用单调时钟：帧间隔照实、时间轴永远递增。
    // +1 兜底：两帧落在同一微秒时保证严格递增（编码器会丢弃时间戳相等的帧）。
    int64_t timestampUs = webrtc::TimeMicros();
    if (timestampUs <= m_lastTimestampUs)
        timestampUs = m_lastTimestampUs + 1;
    m_lastTimestampUs = timestampUs;

    mapped.unmap();

    if (ret != 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_ERROR("VideoSource") << "帧格式转换失败，libyuv 返回" << ret
                                     << " pixelFormat=" << pixelFormat;
        }
        return;
    }

    webrtc::VideoRotation rotation = webrtc::kVideoRotation_0;
    if (rotationAngle == 90)
        rotation = webrtc::kVideoRotation_90;
    else if (rotationAngle == 180)
        rotation = webrtc::kVideoRotation_180;
    else if (rotationAngle == 270)
        rotation = webrtc::kVideoRotation_270;

    pushFrame(webrtc::VideoFrame::Builder()
                  .set_video_frame_buffer(buffer)
                  .set_rotation(rotation)
                  .set_timestamp_us(timestampUs)
                  .build());
}

// ============================
// VideoSourceInterface
// ============================

// webrtcmanager执行link->addLocalVideoTrack(m_localVideoTrack)时webrtc会自动调用该函数
void VideoTrackSource::AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink,
                                       const webrtc::VideoSinkWants &wants) {
    if (sink == nullptr)
        return;

    // 编码器在这里注册自己。同一根轨道重复注册时本函数会被再次调用，
    // 所以按指针去重，否则计数会越加越多。
    if (m_registeredSinks.insert(sink).second) {
        ++m_sinkCount;
        LOG_INFO("VideoSource") << "编码器已接入,当前订阅者 =" << m_sinkCount;
    }

    m_broadcaster.AddOrUpdateSink(sink, wants);
}

void VideoTrackSource::RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink) {
    if (m_registeredSinks.erase(sink) > 0)
        --m_sinkCount;

    m_broadcaster.RemoveSink(sink);
}

void VideoTrackSource::RequestRefreshFrame() {
    // 上游是本地文件，新订阅者下一帧就会拿到，不需要主动要。
    // (直播场景才需要让摄像头立刻编一个关键帧。)
}

// ============================
// MediaSourceInterface
// ============================

VideoTrackSource::SourceState VideoTrackSource::state() const {
    // kLive 表示"源是活的、可以随时出帧"。
    // 用 kEnded 会让 WebRTC 认为流已结束，不再发送。
    return m_hasReceivedFrame ? kLive : kInitializing;
}

bool VideoTrackSource::remote() const {
    // 这是本地源，不是从远端收到的
    return false;
}

// ============================
// VideoTrackSourceInterface
// ============================

bool VideoTrackSource::is_screencast() const {
    // 本端发送的是解码后的视频帧，并非屏幕采集。
    // 该标志会影响编码器的码率策略，取值错误会导致画质异常。
    return false;
}

std::optional<bool> VideoTrackSource::needs_denoising() const {
    // 源视频已经是重编码过的，没有传感器噪声，不需要降噪
    return false;
}

bool VideoTrackSource::GetStats(Stats *stats) {
    if (stats == nullptr)
        return false;

    stats->input_width = m_width;
    stats->input_height = m_height;
    return true;
}

bool VideoTrackSource::SupportsEncodedOutput() const {
    // 本端喂入的是原始帧，不支持直接输出编码后的帧
    return false;
}

void VideoTrackSource::GenerateKeyFrame() {
    // 关键帧的生成时机由编码器决定。上游为原始帧，不具备「强制下一帧为关键帧」
    // 的能力，那属于编码器职责。
}

void VideoTrackSource::AddEncodedSink(
    webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame> *sink) {
    // 仅在 SupportsEncodedOutput() 为 true 时有意义，本类返回 false
}

void VideoTrackSource::RemoveEncodedSink(
    webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame> *sink) {
}
