//
// 远端视频渲染：接收侧，VideoTrackSource 的镜像。
//
//   发送侧:  QVideoFrame ──转换──→ webrtc::VideoFrame → 编码器
//   接收侧:  解码器 → webrtc::VideoFrame ──转换──→ QVideoFrame → QVideoSink
//
// 线程：帧由 WebRTC 解码线程到达。OnFrame 只入队；专用 worker 做
// ToI420 + QVideoFrame 组装；GUI 线程只做排期与 setVideoFrame。
//
// 音画同步：观众端音频还要经过 QAudioSink 的 100ms 缓冲和环形队列，视频不能
// “收到就画”。这里按 `到达时间 + 当前音频积压延迟` 排期，延迟由
// MovieAudioPlayer::playoutDelayMs() 经 WebrtcManager 注入，取不到时退化为 0。
//

#ifndef SYNCINE_REMOTEVIDEORENDERER_H
#define SYNCINE_REMOTEVIDEORENDERER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include <QObject>
#include <QPointer>
#include <QVideoFrame>

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

    // 清空待显示/待转换队列（远端 seek 时调用）。
    void resetQueue();

    // ---- VideoSinkInterface ----
    // 在 WebRTC 解码线程被调用
    void OnFrame(const webrtc::VideoFrame &frame) override;
    void OnDiscardedFrame() override;

    int frameCount() const {
        return m_frameCount.load(std::memory_order_relaxed);
    }

private:
    struct IncomingFrame {
        IncomingFrame(const webrtc::VideoFrame &frame_, int64_t arrival,
                      int64_t mediaTimestamp)
            : frame(frame_), arrivalUs(arrival), mediaTimestampUs(mediaTimestamp) {}

        webrtc::VideoFrame frame;
        int64_t arrivalUs = 0;
        int64_t mediaTimestampUs = 0;
    };

    struct PendingFrame {
        QVideoFrame qtFrame;
        int64_t arrivalUs = 0;
        int64_t mediaTimestampUs = 0;
    };

    // worker 线程：取帧 → ToI420/组装 → 投递到 GUI
    void workerLoop();
    // worker/GUI 回退路径共用：把一帧 WebRTC 画面转成 QVideoFrame。
    std::optional<QVideoFrame> convertToQtFrame(const webrtc::VideoFrame &frame);
    // GUI 线程：把 worker 已转换的帧搬到排期队列。
    void drainConverted();
    // GUI 线程：worker 关闭时的回退路径，直接在 GUI 转换。
    void scheduleIncomingOnGui(IncomingFrame incoming);
    // 更新到达间隔 vs RTP 时间戳间隔的 EWMA（GUI 线程）。
    void noteFrameTiming(int64_t arrivalUs, int64_t mediaTimestampUs);
    void pushScheduled(QVideoFrame frame, int64_t arrivalUs,
                       int64_t mediaTimestampUs);

    // GUI 线程：按当前延迟启动单次定时器。
    void armTimer();
    // GUI 线程：显示已到点的最新一帧，然后重新定时。
    void deliverDueFrames();
    // GUI 线程：当前平滑后的延迟（毫秒）。每次调用都会推进 EMA。
    int currentDelayMs();
    // GUI 线程：真正把一帧写进 QVideoSink。
    void presentFrame(const QVideoFrame &frame);

    void stopWorker();

    std::atomic<int> m_frameCount{0};
    std::atomic<int> m_discardedCount{0};
    // 因队列上限/过晚而丢弃的帧数
    std::atomic<int> m_droppedForLag{0};

    // worker 队列（WebRTC 帧）。GUI 线程 resetQueue 时也会清空它。
    std::thread m_worker;
    bool m_workerEnabled = true;
    std::atomic<bool> m_workerStarted{false};
    std::mutex m_workerMutex;
    std::condition_variable m_workerCv;
    std::deque<IncomingFrame> m_workerQueue;
    bool m_stopWorker = false;

    // worker 已转换、等待 GUI 搬运的帧。**必须有界**：如果每帧都 post 一个
    // lambda，GUI 忙时事件队列会无限堆积 4K QVideoFrame（每帧约 12MB），
    // 内存可以涨到数 GB。这里只保留一个待处理的唤醒事件。
    std::mutex m_convertedMutex;
    std::deque<PendingFrame> m_converted;
    size_t m_convertedBytes = 0;
    bool m_convertedPosted = false;

    // resetQueue 递增；worker 投递结果时据此丢弃 reset 前的旧帧。
    std::atomic<uint64_t> m_generation{0};

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

    // 到达抖动：到达间隔与 RTP 时间戳间隔之差的 EWMA。只用于决定视频
    // 额外缓冲，不改变音频侧 NetEq 的行为。
    double m_jitterMs = 0.0;
    int64_t m_lastMediaTimestampUs = 0;
    int64_t m_lastArrivalUs = 0;
    // 有电影声时的最小视频缓冲；SYNCINE_SHARE_JITTER_MS 可覆盖。
    double m_jitterFloorMs = 100.0;

    std::atomic<bool> m_writingEnabled{true};

    // QVideoSink 的生命周期由 QML 掌管，这里只借用
    QPointer<QVideoSink> m_targetSink;
};

#endif //SYNCINE_REMOTEVIDEORENDERER_H
