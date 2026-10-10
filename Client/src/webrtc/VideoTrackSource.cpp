//
// Created by donggu on 2026/9/16.
//

#include "VideoTrackSource.h"

#include <QDebug>
#include <QVideoFrame>
#include <QVideoFrameFormat>

#include "core/Log.h"

#include "api/video/i420_buffer.h"
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
        // Y 平面 + VU 交错平面(和 NV12 的 UV 顺序相反)
        return libyuv::FOURCC_NV21;
    case QVideoFrameFormat::Format_YUYV:
        return libyuv::FOURCC_YUY2;
    default:
        return 0;
    }
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

    // 广播给所有订阅者(编码器) —— 他们会各自去编码,然后发 RTP
    m_broadcaster.OnFrame(frame);
}

void VideoTrackSource::pushQtFrame(const QVideoFrame &frame) {
    if (!frame.isValid())
        return;

    // map() 不是 const 方法,而且必须和 unmap() 配对。
    // 拷一份出来(隐式共享,很廉价),并保证所有出口都会 unmap。
    QVideoFrame mapped = frame;

    const uint32_t fourcc = toLibyuvFourcc(mapped.surfaceFormat().pixelFormat());
    if (fourcc == 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_ERROR("VideoSource") << "不支持的像素格式"
                                     << static_cast<int>(mapped.surfaceFormat().pixelFormat())
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

    // 诊断:把源帧的真实像素格式和行距打出来。
    //
    // 为什么值得单独打:画面"斜切/撕裂"是行距算错的典型症状,而颜色错位是
    // 平面偏移算错的症状 —— 两者都不报错,只能靠这几个数字定位。
    // Qt 6.5 起媒体后端由 GStreamer 换成 FFmpeg,同一个文件给出的像素格式
    // 可能和以前不同(见 CMakePresets 里的迁移说明)。
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

        // 决定性的一组数字:UV 平面的实际偏移 vs libyuv 会去推算的偏移。
        // 两者不等,就说明"按显示高度推算 UV 位置"必然读错 —— 这正是上面
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
    // 而 Qt 给的帧行距往往更大(内存对齐填充),所以不能直接把 Qt 的
    // 缓冲区交给 WebRTC —— 必须按行搬一遍,把填充去掉。
    //
    // 行距传错的症状是画面逐行错位(斜切),而且不会有任何报错。
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

    if (fourcc == libyuv::FOURCC_I420) {
        // 源已经是 I420(三平面分开),只是行距可能比宽度大(填充)。
        //
        // **必须用 I420Copy,不能用 ConvertToI420。**
        // 后者在 FOURCC_I420 模式下会把三个平面当成"紧挨着连续存放",
        // 自己去算平面偏移 —— 于是忽略你传的行距,把填充字节当成内容读。
        // 症状很典型:第 0 行对、从第 1 行起全错。
        // I420Copy 明确按行距逐行搬,不做四字码解析,才是这种情况的正确工具。
        ret = libyuv::I420Copy(
            srcY, srcStrideY,
            srcU, srcStrideU,
            srcV, srcStrideV,
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            width, height);
    } else if (fourcc == libyuv::FOURCC_NV12) {
        // NV12:一个 Y 平面 + 一个 UV 交错平面。
        //
        // **必须显式传 UV 指针,不能用 ConvertToI420。**
        //
        // ConvertToI420 会自己去算 UV 平面的位置,规则是
        //     src_uv = src_y + src_stride_y * src_height
        // 这个公式只在"解码缓冲的行数恰好等于显示高度"时成立。
        //
        // 而 H.264 的宏块是 16×16,解码器按编码尺寸(width/height 向上取整到
        // 16 的倍数)分配缓冲。一旦显示尺寸不是 16 的倍数(例如 1916×1036),
        // 缓冲的行数就比显示高度多几行,UV 的实际位置相应前移 ——
        // 而 libyuv 还按显示高度去算,读到的位置就落在 Y 数据里,
        // 表现为色块错位、甚至整幅斜切。
        //
        // 两个实测样本正好印证:
        //   2560×1600(宽高都是 16 的倍数,缓冲行数 == 显示高度)→ 基本能看,但有色偏
        //   1916×1036(不是 16 的倍数,缓冲 1920×1040)→ 完全花掉
        //
        // NV12ToI420 直接收 UV 指针,不做任何假设,所以两种情况都对。
        ret = libyuv::NV12ToI420(
            srcY, srcStrideY,
            srcU, srcStrideU,           // ← 用 Qt 给的实际 UV 指针,而不是推算出来的
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            width, height);
    } else if (fourcc == libyuv::FOURCC_NV21) {
        // NV21 与 NV12 只差 UV 两个分量的先后顺序,同样的道理。
        ret = libyuv::NV21ToI420(
            srcY, srcStrideY,
            srcU, srcStrideU,
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            width, height);
    } else {
        // YUY2 是单平面打包格式,没有第二个平面可传,不存在上面那个问题。
        ret = libyuv::YUY2ToI420(
            srcY, srcStrideY,
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            width, height);
    }

    const int rotationAngle = mapped.rotationAngle();

    // ★ 时间戳不能用媒体的 startTime,必须用单调时钟。
    //
    //   startTime 是"媒体内的时间":换源/重播时从 0 重新开始,
    //   往回 seek 时直接跳回更早的值 —— 而 RTP 时间轴要求单调递增。
    //   时间戳一旦回退:
    //     · 播放中往回 seek → 观众解码器把新帧当"旧帧"丢弃,
    //       直到时间轴追上(几十秒后才恢复)→ 表现为卡住
    //     · 播完重播 / 换源 → 从 120s 跳回 0,永远追不上 → 永久卡死
    //     · 暂停时 seek 没事,是因为断流期间解码器重置了时间基准
    //
    //   本应用的画面同步靠控制消息(play/pause/seek),不依赖 RTP 时间戳,
    //   所以直接改用单调时钟:帧间隔照实,时间轴永远递增,
    //   换源、重播、任意方向 seek 都不会打断观众端。
    //
    //   +1 兜底:极端情况下两帧落在同一微秒,保证严格递增
    //   (编码器会丢弃时间戳相等的帧)。
    int64_t timestampUs = webrtc::TimeMicros();
    if (timestampUs <= m_lastTimestampUs)
        timestampUs = m_lastTimestampUs + 1;
    m_lastTimestampUs = timestampUs;

    mapped.unmap();

    if (ret != 0) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_ERROR("VideoSource") << "帧格式转换失败,返回" << ret;
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

    // 编码器在这里注册自己。同一根轨道重复注册时本函数会被再次调用,
    // 所以按指针去重,否则计数会越加越多。
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
    // 上游是本地文件,新订阅者下一帧就会拿到,不需要主动要。
    // (直播场景才需要让摄像头立刻编一个关键帧。)
}

// ============================
// MediaSourceInterface
// ============================

VideoTrackSource::SourceState VideoTrackSource::state() const {
    // kLive 表示"源是活的、可以随时出帧"。
    // 用 kEnded 会让 WebRTC 认为流已结束,不再发送。
    return m_hasReceivedFrame ? kLive : kInitializing;
}

bool VideoTrackSource::remote() const {
    // 这是本地源,不是从远端收到的
    return false;
}

// ============================
// VideoTrackSourceInterface
// ============================

bool VideoTrackSource::is_screencast() const {
    // 我们发的是解码后的视频帧,不是屏幕采集。
    // 这个标志会影响编码器的码率策略,选错会让画质异常。
    return false;
}

std::optional<bool> VideoTrackSource::needs_denoising() const {
    // 源视频已经是重编码过的,没有传感器噪声,不需要降噪
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
    // 我们喂的是原始帧,不支持直接输出编码后的帧
    return false;
}

void VideoTrackSource::GenerateKeyFrame() {
    // 关键帧由编码器决定何时生成。上游是原始帧,没有"强制下一个帧是关键帧"
    // 的能力 —— 那是编码器的事。
}

void VideoTrackSource::AddEncodedSink(
    webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame> *sink) {
    // 仅当 SupportsEncodedOutput() 为 true 时才有意义,我们是 false
}

void VideoTrackSource::RemoveEncodedSink(
    webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame> *sink) {
}
