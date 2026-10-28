//
// Created by donggu on 2026/9/16.
//

#include "VideoTrackSource.h"

#include <QDebug>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QtGlobal>

#include "core/Log.h"

#include "api/video/i420_buffer.h"
#include "api/video/nv12_buffer.h"
#include "libyuv/convert.h"
#include "libyuv/scale.h"
#include "libyuv/video_common.h"
#include "rtc_base/time_utils.h"

namespace {

// 队列上限：1080p 30fps 下 5 帧约 150ms，够吸收一次转换抖动；
// 再多说明转换/编码跟不上，丢帧比延迟增长好。
constexpr size_t kMaxQueuedJobs = 5;

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

VideoTrackSource::VideoTrackSource() {
    // 默认启用 worker；SYNCINE_VIDEO_WORKER=0 回退到 GUI 同步转换，便于排查。
    m_workerEnabled = !(qEnvironmentVariableIsSet("SYNCINE_VIDEO_WORKER")
                        && qEnvironmentVariableIntValue("SYNCINE_VIDEO_WORKER") == 0);

    if (m_workerEnabled) {
        m_worker = std::thread(&VideoTrackSource::workerLoop, this);
        m_workerStarted = true;
    }
}

VideoTrackSource::~VideoTrackSource() {
    shutdown();
}

void VideoTrackSource::setMaxEncodeHeight(int height) {
    m_maxEncodeHeight.store(qMax(0, height), std::memory_order_relaxed);
}

void VideoTrackSource::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopWorker = true;
    }
    m_queueCv.notify_all();

    if (m_worker.joinable())
        m_worker.join();
    m_workerStarted = false;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_jobs.clear();
    }
}

// ============================
// 喂帧
// ============================

void VideoTrackSource::pushFrame(const webrtc::VideoFrame &frame) {
    if (frame.video_frame_buffer() == nullptr)
        return;

    m_width.store(frame.width(), std::memory_order_relaxed);
    m_height.store(frame.height(), std::memory_order_relaxed);
    m_hasReceivedFrame.store(true, std::memory_order_relaxed);

    // 广播给所有订阅者（编码器） —— 他们会各自去编码，然后发 RTP
    m_broadcaster.OnFrame(frame);
}

int64_t VideoTrackSource::nextTimestampUs() {
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
    return timestampUs;
}

void VideoTrackSource::pushQtFrame(const QVideoFrame &frame) {
    if (!frame.isValid())
        return;

    // 没有订阅者（没有对端/观众）时不做任何转换与分配。
    if (sinkCount() == 0)
        return;

    // 时间戳在 GUI 线程到达时打；worker 排队不能改变 RTP 时间轴。
    const int64_t timestampUs = nextTimestampUs();

    if (!m_workerStarted) {
        // 回退开关：完全同步转换（旧行为）。
        if (auto converted = convertQtFrame(frame, timestampUs))
            pushFrame(*converted);
        return;
    }

    Job job;
    job.timestampUs = timestampUs;

    if (frame.handleType() == QVideoFrame::NoHandle) {
        // CPU 帧：把它交给 worker map，GUI 只递一个隐式共享的句柄。
        job.kind = JobKind::QtFrame;
        job.qtFrame = frame;
    } else {
        // GPU 帧的 map 线程安全性取决于后端；留在 GUI 转换，但转换结果仍然
        // 进入同一个 FIFO，避免它插到更老的 worker 帧前面。
        auto converted = convertQtFrame(frame, timestampUs);
        if (!converted)
            return;
        job.kind = JobKind::ReadyFrame;
        job.readyFrame = std::move(*converted);
    }

    enqueueJob(std::move(job));
}

void VideoTrackSource::enqueueJob(Job job) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_jobs.size() >= kMaxQueuedJobs) {
            m_jobs.pop_front();
            m_droppedJobs.fetch_add(1, std::memory_order_relaxed);
        }
        m_jobs.push_back(std::move(job));
    }
    m_queueCv.notify_one();
}

void VideoTrackSource::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this] {
                return m_stopWorker || !m_jobs.empty();
            });
            if (m_stopWorker)
                break;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }

        if (job.kind == JobKind::QtFrame) {
            if (auto converted = convertQtFrame(job.qtFrame, job.timestampUs))
                pushFrame(*converted);
        } else if (job.readyFrame.has_value()) {
            pushFrame(*job.readyFrame);
        }

        // 每 300 帧一条 Info（24fps 下约 12 秒）：排查“推流卡/内存涨”时，
        // 队列深度和丢帧数比逐帧日志有用得多。
        const int pushed = m_pushedFrames.fetch_add(1, std::memory_order_relaxed) + 1;
        if (pushed % 300 == 0) {
            size_t queued = 0;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                queued = m_jobs.size();
            }
            LOG_INFO("VideoSource") << "已推送" << pushed << "帧, 队列" << queued
                                    << "丢帧" << m_droppedJobs.load()
                                    << "订阅者" << sinkCount();
        }
    }
}

// ============================
// Qt 帧 → I420
// ============================

std::optional<webrtc::VideoFrame> VideoTrackSource::convertQtFrame(
    const QVideoFrame &frame, int64_t timestampUs) {
    if (!frame.isValid())
        return std::nullopt;

    // map() 不是 const 方法，而且必须和 unmap() 配对。
    // 拷一份出来（隐式共享，很廉价）,并保证所有出口都会 unmap。
    QVideoFrame mapped = frame;

    const QVideoFrameFormat::PixelFormat pixelFormat = mapped.surfaceFormat().pixelFormat();
    const uint32_t fourcc = toLibyuvFourcc(pixelFormat);
    const bool tenBit = isTenBit(pixelFormat);

    if (fourcc == 0 && !tenBit) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            LOG_ERROR("VideoSource") << "不支持的像素格式"
                                     << static_cast<int>(pixelFormat)
                                     << "—— 这一路视频不会发送。需要为它补一条到 libyuv 的映射。";
        }
        return std::nullopt;
    }

    if (!mapped.map(QVideoFrame::ReadOnly))
        return std::nullopt;

    const int width = mapped.width();
    const int height = mapped.height();

    if (width <= 0 || height <= 0) {
        mapped.unmap();
        return std::nullopt;
    }

    // 诊断：记录源帧的真实像素格式与行距。
    // 画面「斜切/撕裂」是行距算错的典型症状，颜色错位则是平面偏移算错的症状，
    // 两者均不报错，只能依靠这几个数值定位。Qt 6.5 起媒体后端由 GStreamer
    // 改为 FFmpeg，同一文件给出的像素格式可能与以往不同（见 CMakePresets）。
    static std::atomic<int> diagCount{0};
    const int diag = diagCount.fetch_add(1, std::memory_order_relaxed);
    if (diag < 3 || diag % 300 == 0) {
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

    // WebRTC 要的是**紧凑**的 I420:每个平面的行距等于宽度。
    // 而 Qt 给的帧行距往往更大（内存对齐填充）,所以不能直接把 Qt 的
    // 缓冲区交给 WebRTC —— 必须按行搬一遍，把填充去掉。
    //
    // 行距传错的症状是画面逐行错位（斜切）,而且不会有任何报错。
    const uint8_t *srcY = mapped.bits(0);
    const int srcStrideY = mapped.bytesPerLine(0);
    const uint8_t *srcU = mapped.planeCount() > 1 ? mapped.bits(1) : nullptr;
    const int srcStrideU = mapped.planeCount() > 1 ? mapped.bytesPerLine(1) : 0;
    const uint8_t *srcV = mapped.planeCount() > 2 ? mapped.bits(2) : nullptr;
    const int srcStrideV = mapped.planeCount() > 2 ? mapped.bytesPerLine(2) : 0;

    // 编码前缩放：4K 源按目标短边（默认档位 1080p）缩到目标尺寸。
    // 目的不是画质，而是避免每帧对 4096×2160 做完整 I420 转换/分配，
    // 那会在 Windows 等机器上把 CPU/内存拖垮。
    int outWidth = width;
    int outHeight = height;
    const int maxEncodeHeight = m_maxEncodeHeight.load(std::memory_order_relaxed);
    if (maxEncodeHeight > 0) {
        const int shortSide = qMin(width, height);
        if (shortSide > maxEncodeHeight) {
            const double factor = static_cast<double>(maxEncodeHeight) / shortSide;
            outWidth = qMax(2, (static_cast<int>(width * factor) + 1) & ~1);
            outHeight = qMax(2, (static_cast<int>(height * factor) + 1) & ~1);
        }
    }
    const bool scaleNeeded = outWidth != width || outHeight != height;

    // 前几帧打 Info：Windows 上 handleType 决定转换是否真的在 worker 里跑。
    // 如果这里不是 NoHandle，说明转换回退到了 GUI 线程，4K 源会明显卡顿。
    static std::atomic<int> frameInfoCount{0};
    if (frameInfoCount.fetch_add(1, std::memory_order_relaxed) < 3) {
        LOG_INFO("VideoSource") << "源帧" << width << "x" << height
                                << "→ 编码" << outWidth << "x" << outHeight
                                << " pixelFormat=" << static_cast<int>(pixelFormat)
                                << " handleType=" << static_cast<int>(mapped.handleType())
                                << (mapped.handleType() == QVideoFrame::NoHandle
                                        ? "(worker 转换)"
                                        : "(GUI 回退转换)");
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> buffer;
    int ret = 0;
    bool scaledDirectly = false;

    // 常见格式直接在源平面上缩放，跳过整帧 4K I420 的分配与搬运。
    if (scaleNeeded && fourcc == libyuv::FOURCC_I420) {
        buffer = webrtc::I420Buffer::Create(outWidth, outHeight);
        ret = libyuv::I420Scale(
            srcY, srcStrideY, srcU, srcStrideU, srcV, srcStrideV, width, height,
            buffer->MutableDataY(), buffer->StrideY(),
            buffer->MutableDataU(), buffer->StrideU(),
            buffer->MutableDataV(), buffer->StrideV(),
            outWidth, outHeight, libyuv::kFilterBox);
        scaledDirectly = true;
    } else if (scaleNeeded && fourcc == libyuv::FOURCC_NV12) {
        auto scaledNv12 = webrtc::NV12Buffer::Create(outWidth, outHeight);
        ret = libyuv::NV12Scale(
            srcY, srcStrideY, srcU, srcStrideU, width, height,
            scaledNv12->MutableDataY(), scaledNv12->StrideY(),
            scaledNv12->MutableDataUV(), scaledNv12->StrideUV(),
            outWidth, outHeight, libyuv::kFilterBox);
        if (ret == 0) {
            buffer = webrtc::I420Buffer::Create(outWidth, outHeight);
            ret = libyuv::NV12ToI420(
                scaledNv12->MutableDataY(), scaledNv12->StrideY(),
                scaledNv12->MutableDataUV(), scaledNv12->StrideUV(),
                buffer->MutableDataY(), buffer->StrideY(),
                buffer->MutableDataU(), buffer->StrideU(),
                buffer->MutableDataV(), buffer->StrideV(),
                outWidth, outHeight);
        }
        scaledDirectly = true;
    } else if (scaleNeeded && fourcc == libyuv::FOURCC_NV21) {
        // libyuv 没有 NV21Scale：Y/UV 平面各自 ScalePlane，UV 仍是 VU 顺序。
        auto scaledNv21 = webrtc::NV12Buffer::Create(outWidth, outHeight);
        const int srcChromaWidth = (width + 1) / 2;
        const int srcChromaHeight = (height + 1) / 2;
        const int dstChromaWidth = (outWidth + 1) / 2;
        const int dstChromaHeight = (outHeight + 1) / 2;
        ret = libyuv::ScalePlane(srcY, srcStrideY, width, height,
                                 scaledNv21->MutableDataY(), scaledNv21->StrideY(),
                                 outWidth, outHeight, libyuv::kFilterBox);
        if (ret == 0) {
            ret = libyuv::ScalePlane(srcU, srcStrideU, srcChromaWidth,
                                     srcChromaHeight,
                                     scaledNv21->MutableDataUV(),
                                     scaledNv21->StrideUV(),
                                     dstChromaWidth, dstChromaHeight,
                                     libyuv::kFilterBox);
        }
        if (ret == 0) {
            buffer = webrtc::I420Buffer::Create(outWidth, outHeight);
            ret = libyuv::NV21ToI420(
                scaledNv21->MutableDataY(), scaledNv21->StrideY(),
                scaledNv21->MutableDataUV(), scaledNv21->StrideUV(),
                buffer->MutableDataY(), buffer->StrideY(),
                buffer->MutableDataU(), buffer->StrideU(),
                buffer->MutableDataV(), buffer->StrideV(),
                outWidth, outHeight);
        }
        scaledDirectly = true;
    }

    if (!scaledDirectly) {
        buffer = webrtc::I420Buffer::Create(width, height);
    }

    if (!scaledDirectly) {
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

        // 10bit / YUY2 等少见的格式：先按原尺寸转 I420，再缩到目标尺寸。
        if (ret == 0 && scaleNeeded) {
            webrtc::scoped_refptr<webrtc::I420Buffer> scaled =
                webrtc::I420Buffer::Create(outWidth, outHeight);
            ret = libyuv::I420Scale(
                buffer->DataY(), buffer->StrideY(),
                buffer->DataU(), buffer->StrideU(),
                buffer->DataV(), buffer->StrideV(), width, height,
                scaled->MutableDataY(), scaled->StrideY(),
                scaled->MutableDataU(), scaled->StrideU(),
                scaled->MutableDataV(), scaled->StrideV(),
                outWidth, outHeight, libyuv::kFilterBox);
            if (ret == 0)
                buffer = scaled;
        }
    } // !scaledDirectly

    const int rotationAngle = mapped.rotationAngle();

    mapped.unmap();

    if (ret != 0) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            LOG_ERROR("VideoSource") << "帧格式转换失败，libyuv 返回" << ret
                                     << " pixelFormat=" << pixelFormat;
        }
        return std::nullopt;
    }

    webrtc::VideoRotation rotation = webrtc::kVideoRotation_0;
    if (rotationAngle == 90)
        rotation = webrtc::kVideoRotation_90;
    else if (rotationAngle == 180)
        rotation = webrtc::kVideoRotation_180;
    else if (rotationAngle == 270)
        rotation = webrtc::kVideoRotation_270;

    return webrtc::VideoFrame::Builder()
        .set_video_frame_buffer(buffer)
        .set_rotation(rotation)
        .set_timestamp_us(timestampUs)
        .build();
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
    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        inserted = m_registeredSinks.insert(sink).second;
        if (inserted)
            m_sinkCount.fetch_add(1, std::memory_order_relaxed);
    }
    if (inserted) {
        LOG_DEBUG("VideoSource") << "编码器已接入,当前订阅者 ="
                                << m_sinkCount.load();
    }

    m_broadcaster.AddOrUpdateSink(sink, wants);
}

void VideoTrackSource::RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink) {
    {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        if (m_registeredSinks.erase(sink) > 0)
            m_sinkCount.fetch_sub(1, std::memory_order_relaxed);
    }

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
    return m_hasReceivedFrame.load(std::memory_order_relaxed) ? kLive : kInitializing;
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

    stats->input_width = m_width.load(std::memory_order_relaxed);
    stats->input_height = m_height.load(std::memory_order_relaxed);
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
