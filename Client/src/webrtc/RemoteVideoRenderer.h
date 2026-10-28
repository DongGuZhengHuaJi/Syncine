//
// 远端视频渲染：接收侧，VideoTrackSource 的镜像。
//
//   发送侧:  QVideoFrame ──转换──→ webrtc::VideoFrame → 编码器
//   接收侧:  解码器 → webrtc::VideoFrame ──转换──→ QVideoFrame → QVideoSink
//
// 需要适配器的原因：WebRTC 的 VideoSinkInterface 与 Qt 的 QVideoSink 是两套不同的
// 接口，且 WebRTC 提供的是 I420 平面数据（带行距填充），Qt 的 QVideoFrame 需要自身的
// 格式描述，转换只能在此完成。
//
// 线程：帧由 WebRTC 解码线程到达，而 QVideoSink 归属 GUI 线程。OnFrame 只入队；
// 转换、排期与投递都在 GUI 线程执行（阶段 5 再把转换搬到 worker）。
//
// 音画同步：观众端音频还要经过 QAudioSink 的 100ms 缓冲和环形队列，视频不能
// “收到就画”。这里按 `到达时间 + 当前音频积压延迟` 排期，延迟由
// MovieAudioPlayer::playoutDelayMs() 经 WebrtcManager 注入，取不到时退化为 0。
//

#ifndef SYNCINE_REMOTEVIDEORENDERER_H
#define SYNCINE_REMOTEVIDEORENDERER_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>

#include <QObject>
#include <QPointer>

#include "api/media_stream_interface.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"

class QTimer;
class QVideoSink;

class RemoteVideoRenderer : public QObject,
                            public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
    Q_OBJECT

public:
    explicit RemoteVideoRenderer(QObject *parent = nullptr);
    ~RemoteVideoRenderer() override;

    // 画面往哪个 sink 送。由 main.cpp 装配时设成 PlaybackController 的 remoteVideoSink。
    // 用 QPointer：sink 归 QML 的 VideoOutput 所有，页面销毁后不能留裸指针。
    void setTargetSink(QVideoSink *sink);

    // 是否往 sink 写帧。由 main.cpp 装配时设成
    void setWritingEnabled(bool enabled);

    // 当前音频从“WebRTC 交出”到“扬声器响出”的估计延迟（毫秒）。
    // 由 WebrtcManager 注入；不设置时视频不额外延迟。
    void setPlayoutDelayProvider(std::function<int()> provider);

    // 清空待显示队列（远端 seek 时调用）。
    void resetQueue();

    // ---- VideoSinkInterface ----
    // 在 WebRTC 解码线程被调用
    void OnFrame(const webrtc::VideoFrame &frame) override;
    void OnDiscardedFrame() override;

    int frameCount() const {
        return m_frameCount.load(std::memory_order_relaxed);
    }

private:
    struct PendingFrame {
        webrtc::VideoFrame frame;
        int64_t arrivalUs = 0;
    };

    // GUI 线程：把 OnFrame 暂存的帧搬到排期队列，并按当前延迟重新定时。
    void drainIncoming();
    // GUI 线程：显示已经到点的最新一帧，然后重新定时。
    void deliverDueFrames();
    // GUI 线程：按队首帧的到期时间启动单次定时器。
    void armTimer();
    // GUI 线程：当前平滑后的延迟（毫秒）。每次调用都会推进 EMA。
    int currentDelayMs();
    // GUI 线程：真正把一帧写进 QVideoSink。
    void renderFrame(const webrtc::VideoFrame &frame);

    std::atomic<int> m_frameCount{0};
    std::atomic<int> m_discardedCount{0};
    // 因队列上限/过晚而丢弃的帧数
    std::atomic<int> m_droppedForLag{0};

    // OnFrame（解码线程）→ GUI 的暂存区
    std::mutex m_frameMutex;
    std::deque<PendingFrame> m_incoming;
    bool m_deliveryPosted = false;

    // GUI 线程独占：等待显示的帧（按到达顺序）
    std::deque<PendingFrame> m_scheduled;
    size_t m_scheduledBytes = 0;
    QTimer *m_presentTimer = nullptr;

    std::function<int()> m_delayProvider;
    // 时间基非对称 EMA：上升快、下降慢；数值只在 GUI 线程访问。
    double m_delayMs = 0.0;
    int64_t m_lastDelayUpdateUs = 0;
    int64_t m_lastAudioUs = 0;
    bool m_hasSeenAudio = false;

    std::atomic<bool> m_writingEnabled{true};

    // QVideoSink 的生命周期由 QML 掌管，这里只借用
    QPointer<QVideoSink> m_targetSink;
};

#endif //SYNCINE_REMOTEVIDEORENDERER_H
