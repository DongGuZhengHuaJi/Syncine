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
#include <QtGlobal>

#include "core/Log.h"

#include "api/video/i420_buffer.h"

namespace {

// 排队上限。1080p I420 一帧约 3.1MB：24 帧 ≈ 74MB，再用 64MB 的字节上限兜底。
constexpr int kMaxWorkerFrames = 24;
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

size_t frameBytes(const QVideoFrame &frame)
{
    if (!frame.isValid())
        return 0;
    return static_cast<size_t>(frame.width()) * static_cast<size_t>(frame.height()) * 3 / 2;
}

} // namespace

RemoteVideoRenderer::RemoteVideoRenderer(QObject *parent)
    : QObject(parent) {
    m_presentTimer = new QTimer(this);
    m_presentTimer->setSingleShot(true);
    m_presentTimer->setTimerType(Qt::PreciseTimer);
    connect(m_presentTimer, &QTimer::timeout, this, &RemoteVideoRenderer::deliverDueFrames);

    // 默认启用转换 worker；SYNCINE_RENDER_WORKER=0 回退到 GUI 转换。
    m_workerEnabled = !(qEnvironmentVariableIsSet("SYNCINE_RENDER_WORKER")
                        && qEnvironmentVariableIntValue("SYNCINE_RENDER_WORKER") == 0);
    if (m_workerEnabled) {
        m_worker = std::thread(&RemoteVideoRenderer::workerLoop, this);
        m_workerStarted.store(true);
    }
}

RemoteVideoRenderer::~RemoteVideoRenderer() {
    stopWorker();
    if (m_presentTimer != nullptr)
        m_presentTimer->stop();
}

void RemoteVideoRenderer::stopWorker() {
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_stopWorker = true;
    }
    m_workerCv.notify_all();

    if (m_worker.joinable())
        m_worker.join();
    m_workerStarted.store(false);

    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_workerQueue.clear();
    }
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
    // 代次递增：worker 里正在转换、还没投递的旧帧到达 GUI 时会被丢弃。
    m_generation.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_workerQueue.clear();
    }
    m_scheduled.clear();
    m_scheduledBytes = 0;
    if (m_presentTimer != nullptr)
        m_presentTimer->stop();
}

// ============================
// 接收侧：解码线程 → worker → GUI
// ============================

void RemoteVideoRenderer::OnFrame(const webrtc::VideoFrame &frame) {
    if (!m_writingEnabled.load(std::memory_order_relaxed))
        return;

    IncomingFrame incoming{frame, steadyNowUs()};

    if (!m_workerStarted.load(std::memory_order_relaxed)) {
        // 回退开关：在 GUI 线程转换（旧行为）。
        QMetaObject::invokeMethod(this,
                                  [this, incoming = std::move(incoming)]() mutable {
                                      scheduleIncomingOnGui(std::move(incoming));
                                  },
                                  Qt::QueuedConnection);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        if (static_cast<int>(m_workerQueue.size()) >= kMaxWorkerFrames) {
            m_workerQueue.pop_front();
            m_droppedForLag.fetch_add(1, std::memory_order_relaxed);
        }
        m_workerQueue.push_back(std::move(incoming));
    }
    m_workerCv.notify_one();
}

void RemoteVideoRenderer::workerLoop() {
    for (;;) {
        std::optional<IncomingFrame> incoming;
        {
            std::unique_lock<std::mutex> lock(m_workerMutex);
            m_workerCv.wait(lock, [this] {
                return m_stopWorker || !m_workerQueue.empty();
            });
            if (m_stopWorker)
                break;
            incoming.emplace(m_workerQueue.front());
            m_workerQueue.pop_front();
        }

        const uint64_t generation = m_generation.load(std::memory_order_relaxed);
        auto converted = convertToQtFrame(incoming->frame);
        if (!converted)
            continue;

        const int64_t arrivalUs = incoming->arrivalUs;
        QMetaObject::invokeMethod(
            this,
            [this, qtFrame = std::move(*converted), arrivalUs, generation]() mutable {
                scheduleConverted(std::move(qtFrame), arrivalUs, generation);
            },
            Qt::QueuedConnection);
    }
}

std::optional<QVideoFrame> RemoteVideoRenderer::convertToQtFrame(
    const webrtc::VideoFrame &frame) {
    // 统一拿到 I420 视图。收到的可能是 I420/I420A/NV12 等，
    // ToI420() 会在需要时做一次转换，之后按统一方式取平面。
    if (frame.video_frame_buffer() == nullptr)
        return std::nullopt;

    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
        frame.video_frame_buffer()->ToI420();
    if (i420 == nullptr)
        return std::nullopt;

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
        return std::nullopt;

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
    return qtFrame;
}

void RemoteVideoRenderer::scheduleIncomingOnGui(IncomingFrame incoming) {
    if (!m_writingEnabled.load(std::memory_order_relaxed))
        return;

    auto converted = convertToQtFrame(incoming.frame);
    if (!converted)
        return;

    pushScheduled(std::move(*converted), incoming.arrivalUs);
}

void RemoteVideoRenderer::scheduleConverted(QVideoFrame frame, int64_t arrivalUs,
                                            uint64_t generation) {
    // resetQueue 之后到达的旧转换结果直接丢弃。
    if (generation != m_generation.load(std::memory_order_relaxed)
        || !m_writingEnabled.load(std::memory_order_relaxed)) {
        return;
    }

    pushScheduled(std::move(frame), arrivalUs);
}

void RemoteVideoRenderer::pushScheduled(QVideoFrame frame, int64_t arrivalUs) {
    if (m_targetSink.isNull())
        return;

    const size_t bytes = frameBytes(frame);

    if (static_cast<int>(m_scheduled.size()) >= kMaxScheduledFrames
        || m_scheduledBytes + bytes > kMaxScheduledBytes) {
        // 队列满：丢最老的一帧；如果排期队列为空，只能丢新来的这一帧。
        if (!m_scheduled.empty()) {
            m_scheduledBytes -= frameBytes(m_scheduled.front().qtFrame);
            m_scheduled.pop_front();
        } else {
            m_droppedForLag.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        m_droppedForLag.fetch_add(1, std::memory_order_relaxed);
    }

    m_scheduledBytes += bytes;
    m_scheduled.push_back(PendingFrame{std::move(frame), arrivalUs});
    armTimer();
}

void RemoteVideoRenderer::OnDiscardedFrame() {
    // 编码器丢弃了某一帧（通常是网络拥塞时降帧率）。
    // 界面上表现为轻微卡顿，不需要特殊处理 —— 但要能看见发生了。
    if (m_discardedCount.fetch_add(1, std::memory_order_relaxed) % 30 == 0) {
        LOG_DEBUG("RemoteVideo") << "已丢弃" << m_discardedCount.load()
                                 << "帧(拥塞)";
    }
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
            m_scheduledBytes -= frameBytes(m_scheduled[i].qtFrame);
        PendingFrame chosen = std::move(m_scheduled[newestDue]);
        m_scheduled.erase(m_scheduled.begin(), m_scheduled.begin() + newestDue + 1);
        presentFrame(chosen.qtFrame);
    }

    armTimer();
}

void RemoteVideoRenderer::presentFrame(const QVideoFrame &frame) {
    if (m_targetSink.isNull())
        return;

    // 交给界面。QVideoSink 会把它推给正在渲染的那个 VideoOutput。
    m_targetSink->setVideoFrame(frame);

    const int count = m_frameCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count == 1) {
        LOG_INFO("RemoteVideo") << "收到第一帧远端画面:" << frame.width()
                                << "x" << frame.height();
    } else if (count % 300 == 0) {
        LOG_TRACE("RemoteVideo") << "已显示" << count << "帧, 当前延迟"
                                 << m_delayMs << "ms, 排队"
                                 << m_scheduled.size() << "帧, 丢帧"
                                 << m_droppedForLag.load();
    }
}
