//
// Created by donggu on 2026/9/16.
//

#include "RemoteVideoRenderer.h"

#include <cstdio>
#include <cstring>

#include <QMetaObject>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include "core/Log.h"

#include "api/video/i420_buffer.h"

RemoteVideoRenderer::RemoteVideoRenderer(QObject *parent)
    : QObject(parent) {
}

RemoteVideoRenderer::~RemoteVideoRenderer() = default;

void RemoteVideoRenderer::setTargetSink(QVideoSink *sink) {
    m_targetSink = sink;
}

void RemoteVideoRenderer::setWritingEnabled(bool enabled) {
    m_writingEnabled = enabled;
}

// ============================
// 接收侧
// ============================

void RemoteVideoRenderer::OnFrame(const webrtc::VideoFrame &frame) {
    // 这里在 WebRTC 的信令线程上。QVideoSink 必须在其所属线程(GUI)使用,
    // 所以把帧交给 GUI 线程处理。
    // 新帧到来时,只保留最新的一帧,旧的直接丢掉。避免 GUI 线程处理不过来时堆积。
    bool needPost = false;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_pendingFrame = frame;              // 覆盖:旧的直接丢掉
        if (!m_deliveryPosted) {
            m_deliveryPosted = true;
            needPost = true;
        }
    }

    if (needPost) {
        // 用 invokeMethod 而不是 Qt 信号:webrtc::VideoFrame 不是 Qt 类型,
        // 走队列连接需要注册元类型,而这里只是内部转发。
        QMetaObject::invokeMethod(this,
                                  [this]() { deliverPendingFrame(); },
                                  Qt::QueuedConnection);
    }
}

void RemoteVideoRenderer::OnDiscardedFrame() {
    // 编码器丢弃了某一帧(通常是网络拥塞时降帧率)。
    // 界面上表现为轻微卡顿,不需要特殊处理 —— 但要能看见发生了。
    if (++m_discardedCount % 30 == 1) {
        LOG_DEBUG("RemoteVideo") << "已丢弃" << m_discardedCount << "帧(拥塞)";
    }
}

void RemoteVideoRenderer::deliverPendingFrame() {
    // 取出暂存的最新帧。取走后清空标记,允许下一帧再排一次队。
    std::optional<webrtc::VideoFrame> frame;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        frame = std::move(m_pendingFrame);
        m_pendingFrame.reset();
        m_deliveryPosted = false;
    }

    if (!frame.has_value())
        return;                             // 已经被处理过了

    // 显示本地时不渲染远端帧 —— 那些是回声,写进 sink 只会盖掉本地画面。
    if (!m_writingEnabled)
        return;

    // [诊断] 定期打一次,看是"收不到"还是"收到了但没显示"
    static int seen = 0;
    if (seen < 10) {
        LOG_TRACE("RemoteVideo") << "渲染第" << (seen + 1)
                                 << "帧, targetSink =" << (void *) m_targetSink;
    }
    ++seen;

    if (m_targetSink == nullptr)
        return;

    // 统一拿到 I420 视图。收到的可能是 I420/I420A/NV12 等,
    // ToI420() 会在需要时做一次转换,之后按统一方式取平面。
    webrtc::scoped_refptr<webrtc::I420BufferInterface> i420 =
        frame->video_frame_buffer()->ToI420();
    if (i420 == nullptr)
        return;

    const int width = i420->width();
    const int height = i420->height();
    const int chromaWidth = (width + 1) / 2;
    const int chromaHeight = (height + 1) / 2;

    // ---- 组装一个 Qt 认的 YUV420P 帧 ----
    QVideoFrameFormat format(QSize(width, height),
                             QVideoFrameFormat::Format_YUV420P);

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

    if (m_frameCount++ == 0) {
        LOG_INFO("RemoteVideo") << "收到第一帧远端画面:" << width << "x" << height;
    }
}
