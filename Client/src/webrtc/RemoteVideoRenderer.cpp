//
// Created by donggu on 2026/9/16.
//

#include "RemoteVideoRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <QMetaObject>
#include <QTimer>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include "core/Log.h"

#include "api/video/i420_buffer.h"

namespace {

// 排队上限。1080p I420 一帧约 3.1MB：24 帧 ≈ 74MB，再用 64MB 的字节上限兜底。
constexpr int kMaxIncomingFrames = 24;
constexpr int kMaxScheduledFrames = 24;
constexpr size_t kMaxScheduledBytes = 64 * 1024 * 1024;
constexpr double kMaxDelayMs = 300.0;
// 延迟平滑的时间常数：上升快（追上音频积压），下降慢（避免来回抖）。
constexpr double kRiseTauSec = 0.18;
constexpr double kFallTauSec = 1.2;
// 音频消失后先保持上次延迟，给暂停/恢复留出余量。
constexpr int64_t kAudioHoldUs = 2'000'000;

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

size_t frameBytes(const webrtc::VideoFrame &frame)
{
    const auto *buffer = frame.video_frame_buffer().get();
    if (buffer == nullptr)
        return 0;
    const size_t pixels = static_cast<size_t>(buffer->width())
                          * static_cast<size_t>(buffer->height());
    return pixels * 3 / 2;
}

} // namespace

RemoteVideoRenderer::RemoteVideoRenderer(QObject *parent)
    : QObject(parent) {
    m_presentTimer = new QTimer(this);
    m_presentTimer->setSingleShot(true);
    m_presentTimer->setTimerType(Qt::PreciseTimer);
    connect(m_presentTimer, &QTimer::timeout, this, &RemoteVideoRenderer::deliverDueFrames);
}

RemoteVideoRenderer::~RemoteVideoRenderer() {
    if (m_presentTimer != nullptr)
        m_presentTimer->stop();
    resetQueue();
}

void RemoteVideoRenderer::setTargetSink(QVideoSink *sink) {
    m_targetSink = sink;
    if (sink == nullptr)
        resetQueue();
}

void RemoteVideoRenderer::setWritingEnabled(bool enabled) {
    if (m_writingEnabled.exchange(enabled) == enabled)
        return;

    if (!enabled) {
        resetQueue();
        return;
    }

    // 从“显示本地”切回“显示远端”：延迟从头开始估算。
    m_delayMs = 0.0;
    m_lastDelayUpdateUs = 0;
    m_lastAudioUs = 0;
    m_hasSeenAudio = false;
}

void RemoteVideoRenderer::setPlayoutDelayProvider(std::function<int()> provider) {
    m_delayProvider = std::move(provider);
    m_delayMs = 0.0;
    m_lastDelayUpdateUs = 0;
    m_lastAudioUs = 0;
    m_hasSeenAudio = false;
}

void RemoteVideoRenderer::resetQueue() {
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_incoming.clear();
        m_deliveryPosted = false;
    }
    m_scheduled.clear();
    m_scheduledBytes = 0;
    if (m_presentTimer != nullptr)
        m_presentTimer->stop();
}

// ============================
// 接收侧：解码线程 → GUI
// ============================

void RemoteVideoRenderer::OnFrame(const webrtc::VideoFrame &frame) {
    if (!m_writingEnabled.load(std::memory_order_relaxed))
        return;

    const PendingFrame pending{frame, steadyNowUs()};

    bool needPost = false;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (static_cast<int>(m_incoming.size()) >= kMaxIncomingFrames) {
            m_incoming.pop_front();
            m_droppedForLag.fetch_add(1, std::memory_order_relaxed);
        }
        m_incoming.push_back(pending);
        if (!m_deliveryPosted) {
            m_deliveryPosted = true;
            needPost = true;
        }
    }

    if (needPost) {
        // 用 invokeMethod 而不是 Qt 信号：webrtc::VideoFrame 不是 Qt 类型，
        // 走队列连接需要注册元类型，而这里只是内部转发。
        QMetaObject::invokeMethod(this,
                                  [this]() { drainIncoming(); },
                                  Qt::QueuedConnection);
    }
}

void RemoteVideoRenderer::OnDiscardedFrame() {
    // 编码器丢弃了某一帧（通常是网络拥塞时降帧率）。
    // 界面上表现为轻微卡顿，不需要特殊处理 —— 但要能看见发生了。
    if (m_discardedCount.fetch_add(1, std::memory_order_relaxed) % 30 == 0) {
        LOG_DEBUG("RemoteVideo") << "已丢弃" << m_discardedCount.load()
                                 << "帧(拥塞)";
    }
}

void RemoteVideoRenderer::drainIncoming() {
    std::deque<PendingFrame> incoming;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        incoming.swap(m_incoming);
        m_deliveryPosted = false;
    }

    if (!m_writingEnabled.load(std::memory_order_relaxed)) {
        armTimer();
        return;
    }

    for (PendingFrame &pending : incoming) {
        const size_t bytes = frameBytes(pending.frame);

        if (static_cast<int>(m_scheduled.size()) >= kMaxScheduledFrames
            || m_scheduledBytes + bytes > kMaxScheduledBytes) {
            // 队列满：丢最老的一帧；如果排期队列为空，只能丢新来的这一帧。
            if (!m_scheduled.empty()) {
                m_scheduledBytes -= frameBytes(m_scheduled.front().frame);
                m_scheduled.pop_front();
            } else {
                m_droppedForLag.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            m_droppedForLag.fetch_add(1, std::memory_order_relaxed);
        }

        m_scheduledBytes += bytes;
        m_scheduled.push_back(std::move(pending));
    }

    armTimer();
}

// ============================
// GUI 线程：排期与显示
// ============================

int RemoteVideoRenderer::currentDelayMs() {
    const int64_t now = steadyNowUs();

    double target = 0.0;
    if (m_delayProvider) {
        target = std::clamp(static_cast<double>(m_delayProvider()), 0.0, kMaxDelayMs);
    }

    if (target > 0.0) {
        m_lastAudioUs = now;
        if (!m_hasSeenAudio) {
            // 第一段音频到达：直接吸附到目标值，别让开头半秒唇音故意不同步。
            m_hasSeenAudio = true;
            m_delayMs = target;
            m_lastDelayUpdateUs = now;
            return static_cast<int>(std::lround(m_delayMs));
        }
    }

    if (m_lastDelayUpdateUs == 0) {
        m_lastDelayUpdateUs = now;
        return static_cast<int>(std::lround(m_delayMs));
    }

    double dt = (now - m_lastDelayUpdateUs) / 1'000'000.0;
    if (dt < 0.001)
        dt = 0.001;
    m_lastDelayUpdateUs = now;

    if (!m_hasSeenAudio) {
        // 一直没有电影声：视频不额外延迟。
        target = 0.0;
    } else if (target <= 0.0 && now - m_lastAudioUs < kAudioHoldUs) {
        // 音频刚消失（暂停/缓冲）：保持上一次延迟，恢复时不会瞬间失配。
        return static_cast<int>(std::lround(m_delayMs));
    }

    const double tau = target > m_delayMs ? kRiseTauSec : kFallTauSec;
    const double alpha = 1.0 - std::exp(-dt / tau);
    m_delayMs += (target - m_delayMs) * alpha;
    m_delayMs = std::clamp(m_delayMs, 0.0, kMaxDelayMs);
    return static_cast<int>(std::lround(m_delayMs));
}

void RemoteVideoRenderer::armTimer() {
    if (!m_writingEnabled.load(std::memory_order_relaxed)
        || m_scheduled.empty()) {
        m_presentTimer->stop();
        return;
    }

    if (m_targetSink.isNull()) {
        // sink 已被 QML 销毁（QPointer 自动变空）：别攒着 3MB/帧的旧画面。
        m_scheduled.clear();
        m_scheduledBytes = 0;
        m_presentTimer->stop();
        return;
    }

    const int delayMs = currentDelayMs();
    const int64_t dueUs = m_scheduled.front().arrivalUs
                          + static_cast<int64_t>(delayMs) * 1000;
    const int64_t waitUs = dueUs - steadyNowUs();
    const int waitMs = static_cast<int>(
        std::clamp<int64_t>(waitUs / 1000, 0, 1000));
    m_presentTimer->start(waitMs);
}

void RemoteVideoRenderer::deliverDueFrames() {
    if (!m_writingEnabled.load(std::memory_order_relaxed) || m_targetSink.isNull()) {
        m_scheduled.clear();
        m_scheduledBytes = 0;
        return;
    }

    const int delayMs = currentDelayMs();
    const int64_t now = steadyNowUs();

    int newestDue = -1;
    for (int i = 0; i < static_cast<int>(m_scheduled.size()); ++i) {
        const int64_t dueUs = m_scheduled[i].arrivalUs
                              + static_cast<int64_t>(delayMs) * 1000;
        if (dueUs <= now)
            newestDue = i;
        else
            break;
    }

    if (newestDue >= 0) {
        // 先按未移动的帧扣字节数，再 move：不然被移走的那帧 buffer 为空，会少扣。
        for (int i = 0; i <= newestDue; ++i)
            m_scheduledBytes -= frameBytes(m_scheduled[i].frame);
        PendingFrame chosen = std::move(m_scheduled[newestDue]);
        m_scheduled.erase(m_scheduled.begin(), m_scheduled.begin() + newestDue + 1);
        renderFrame(chosen.frame);
    }

    armTimer();
}

void RemoteVideoRenderer::renderFrame(const webrtc::VideoFrame &frame) {
    if (m_targetSink.isNull())
        return;

    // 统一拿到 I420 视图。收到的可能是 I420/I420A/NV12 等，
    // ToI420() 会在需要时做一次转换，之后按统一方式取平面。
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
        frame.video_frame_buffer()->ToI420();
    if (i420 == nullptr)
        return;

    const int width = i420->width();
    const int height = i420->height();
    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;

    // ---- 组装一个 Qt 认的 YUV420P 帧 ----
    QVideoFrameFormat format(QSize(width, height),
                             QVideoFrameFormat::Format_YUV420P);

    // 旋转属于**元数据**而非像素：发送端只把旋转角度写入 WebRTC 帧，由渲染方负责应用。
    // 手工组装 QVideoFrame 时必须写回该元数据——否则带旋转信息的竖屏视频，
    // 房主端显示为正向而观众端显示为横向。
    switch (frame.rotation()) {
    case webrtc::kVideoRotation_90:
        format.setRotation(QtVideo::Rotation::Clockwise90);
        break;
    case webrtc::kVideoRotation_180:
        format.setRotation(QtVideo::Rotation::Clockwise180);
        break;
    case webrtc::kVideoRotation_270:
        format.setRotation(QtVideo::Rotation::Clockwise270);
        break;
    default:
        break; // kVideoRotation_0:保持默认
    }

    QVideoFrame qtFrame(format);
    if (!qtFrame.map(QVideoFrame::WriteOnly))
        return;

    // Y 平面
    uchar *dstY = qtFrame.bits(0);
    const int dstStrideY = qtFrame.bytesPerLine(0);
    for (int row = 0; row < height; ++row) {
        std::memcpy(dstY + static_cast<ptrdiff_t>(row) * dstStrideY,
                    i420->DataY() + static_cast<ptrdiff_t>(row) * i420->StrideY(),
                    static_cast<size_t>(width));
    }

    // U 平面
    uchar *dstU = qtFrame.bits(1);
    const int dstStrideU = qtFrame.bytesPerLine(1);
    for (int row = 0; row < chromaHeight; ++row) {
        std::memcpy(dstU + static_cast<ptrdiff_t>(row) * dstStrideU,
                    i420->DataU() + static_cast<ptrdiff_t>(row) * i420->StrideU(),
                    static_cast<size_t>(chromaWidth));
    }

    // V 平面
    uchar *dstV = qtFrame.bits(2);
    const int dstStrideV = qtFrame.bytesPerLine(2);
    for (int row = 0; row < chromaHeight; ++row) {
        std::memcpy(dstV + static_cast<ptrdiff_t>(row) * dstStrideV,
                    i420->DataV() + static_cast<ptrdiff_t>(row) * i420->StrideV(),
                    static_cast<size_t>(chromaWidth));
    }

    qtFrame.unmap();

    // 交给界面。QVideoSink 会把它推给正在渲染的那个 VideoOutput。
    m_targetSink->setVideoFrame(qtFrame);

    const int count = m_frameCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1) {
        LOG_INFO("RemoteVideo") << "收到第一帧远端画面:" << width << "x" << height;
    } else if (count % 300 == 0) {
        LOG_TRACE("RemoteVideo") << "已显示" << count << "帧, 当前延迟"
                                 << m_delayMs << "ms, 排队"
                                 << m_scheduled.size() << "帧, 丢帧"
                                 << m_droppedForLag.load();
    }
}
