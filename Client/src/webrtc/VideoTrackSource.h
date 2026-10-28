//
// 视频轨道的源：帧的入口。
//
// WebRTC 音频侧有现成的 ADM（麦克风采集），视频侧则只提供接口，数据须自行喂入，
// 本类即该喂入口：
//   Qt 解码帧 ──(格式转换)──→ 本类 ──→ 编码器 ──→ RTP（由 VideoBroadcaster 分发）
//
// 线程：GUI 线程只在到达时打时间戳并入队；map/libyuv 转换/I420 分配在专用
// worker 线程完成。所有帧（包括 GPU 帧在 GUI 上的回退转换结果）都走同一个 FIFO，
// 保证进入编码器的时间戳顺序与到达顺序一致。
//
// 生命周期：本类的 Release() 永远返回 kOtherRefsRemained（引用计数不归零），
// 对象**不会**被析构。持有 worker 的类必须显式调用 shutdown()。
//

#ifndef SYNCINE_VIDEOTRACKSOURCE_H
#define SYNCINE_VIDEOTRACKSOURCE_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include <QVideoFrame>

#include "api/media_stream_interface.h"
#include "api/notifier.h"
#include "api/video/video_broadcaster.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "api/video/recordable_encoded_frame.h"

class VideoTrackSource : public webrtc::Notifier<webrtc::VideoTrackSourceInterface> {
public:
    VideoTrackSource();
    ~VideoTrackSource() override;

    VideoTrackSource(const VideoTrackSource &) = delete;
    VideoTrackSource &operator=(const VideoTrackSource &) = delete;

    // 停止 worker 并清空队列。必须在 WebrtcManager 停 WebRTC 线程之前调用；
    // 可重复调用。
    void shutdown();

    // 喂入一帧 WebRTC 格式的帧并广播给所有订阅者（转换已在别处完成时使用）
    void pushFrame(const webrtc::VideoFrame &frame);

    // 喂入一帧 Qt 格式的帧。由 PlaybackController 的 videoFrameAvailable
    // 信号驱动（接线在 main.cpp）。GUI 线程调用。
    void pushQtFrame(const QVideoFrame &frame);

    // 当前订阅者数量
    int sinkCount() const {
        return m_sinkCount.load(std::memory_order_relaxed);
    }

    // 最近一帧的源尺寸；还没收到帧时为 0。画质档位据此决定横/竖屏目标分辨率。
    int inputWidth() const {
        return m_width.load(std::memory_order_relaxed);
    }
    int inputHeight() const {
        return m_height.load(std::memory_order_relaxed);
    }

    // ---- VideoSourceInterface<VideoFrame> ----
    void AddOrUpdateSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink,
                         const webrtc::VideoSinkWants &wants) override;
    void RemoveSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink) override;
    void RequestRefreshFrame() override;

    // ---- MediaSourceInterface ----
    SourceState state() const override;
    bool remote() const override;

    // ---- VideoTrackSourceInterface ----
    bool is_screencast() const override;
    std::optional<bool> needs_denoising() const override;
    bool GetStats(Stats *stats) override;
    bool SupportsEncodedOutput() const override;
    void GenerateKeyFrame() override;
    void AddEncodedSink(webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame> *sink) override;
    void RemoveEncodedSink(webrtc::VideoSinkInterface<webrtc::RecordableEncodedFrame> *sink) override;

    // 生命周期由 WebrtcManager 掌握，Release 永远返回 kOtherRefsRemained
    void AddRef() const override {
        ++m_refCount;
    }

    webrtc::RefCountReleaseStatus Release() const override {
        --m_refCount;
        return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
    }

private:
    enum class JobKind {
        QtFrame,    // worker 负责 map + 转换
        ReadyFrame, // GUI 回退路径已经转换好，worker 只负责按顺序广播
    };

    struct Job {
        JobKind kind = JobKind::QtFrame;
        QVideoFrame qtFrame;
        std::optional<webrtc::VideoFrame> readyFrame;
        int64_t timestampUs = 0;
    };

    // 把一帧 Qt 画面转成 WebRTC I420 帧。timestampUs 必须在 GUI 到达时打，
    // 不能等转换完成后再打（队列等待会改变 RTP 时间轴）。
    std::optional<webrtc::VideoFrame> convertQtFrame(const QVideoFrame &frame,
                                                     int64_t timestampUs);
    // GUI 线程：保证时间戳严格递增。
    int64_t nextTimestampUs();
    void enqueueJob(Job job);
    void workerLoop();

    webrtc::VideoBroadcaster m_broadcaster;

    // 可变的，因为 AddRef/Release 是 const 方法
    mutable std::atomic<int> m_refCount{0};

    // VideoBroadcaster 不暴露订阅者数量，故自行记录。
    // 按指针去重：同一轨道重复注册时 AddOrUpdateSink 也会被调用。
    std::atomic<int> m_sinkCount{0};
    mutable std::mutex m_sinkMutex;
    std::set<webrtc::VideoSinkInterface<webrtc::VideoFrame> *> m_registeredSinks;

    std::atomic<int> m_width{0};
    std::atomic<int> m_height{0};
    std::atomic<bool> m_hasReceivedFrame{false};

    // 上一帧的单调时间戳，用于保证严格递增。只在 GUI 线程访问。
    int64_t m_lastTimestampUs = 0;

    // worker：有界 FIFO，满则丢最老。
    bool m_workerEnabled = true;
    std::thread m_worker;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<Job> m_jobs;
    bool m_stopWorker = false;
    bool m_workerStarted = false;
    std::atomic<int> m_droppedJobs{0};
};

#endif //SYNCINE_VIDEOTRACKSOURCE_H
