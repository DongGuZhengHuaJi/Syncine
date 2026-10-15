//
// 视频轨道的源：帧的入口。
//
// WebRTC 音频侧有现成的 ADM（麦克风采集），视频侧则只提供接口，数据须自行喂入，
// 本类即该喂入口：
//   Qt 解码帧 ──(格式转换)──→ 本类 ──→ 编码器 ──→ RTP（由 VideoBroadcaster 分发）
//
// 当前只做「接收并广播」，不做格式转换——调用方直接提供 webrtc::VideoFrame。
// 先打通链路，待稳定后再加入转换层，出问题时才能区分链路错误与转换错误。
//

#ifndef SYNCINE_VIDEOTRACKSOURCE_H
#define SYNCINE_VIDEOTRACKSOURCE_H

#include <optional>
#include <set>

#include <atomic>

#include "api/media_stream_interface.h"
#include "api/notifier.h"
#include "api/video/video_broadcaster.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "api/video/recordable_encoded_frame.h"

class QVideoFrame;

class VideoTrackSource : public webrtc::Notifier<webrtc::VideoTrackSourceInterface> {
public:
    VideoTrackSource();
    ~VideoTrackSource() override;

    VideoTrackSource(const VideoTrackSource &) = delete;
    VideoTrackSource &operator=(const VideoTrackSource &) = delete;


    // 喂入一帧 WebRTC 格式的帧并广播给所有订阅者
    void pushFrame(const webrtc::VideoFrame &frame);

    // 喂入一帧 Qt 格式的帧。由 PlaybackController 的 videoFrameAvailable
    // 信号驱动（接线在 main.cpp）。
    void pushQtFrame(const QVideoFrame &frame);

    // 当前订阅者数量
    int sinkCount() const {
        return m_sinkCount;
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
    webrtc::VideoBroadcaster m_broadcaster;

    // 可变的，因为 AddRef/Release 是 const 方法
    mutable std::atomic<int> m_refCount{0};

    // VideoBroadcaster 不暴露订阅者数量，故自行记录。
    // 按指针去重：同一轨道重复注册时 AddOrUpdateSink 也会被调用。
    int m_sinkCount = 0;
    std::set<webrtc::VideoSinkInterface<webrtc::VideoFrame> *> m_registeredSinks;

    int m_width = 0;
    int m_height = 0;
    bool m_hasReceivedFrame = false;

    // 上一帧的单调时间戳，用于保证严格递增
    int64_t m_lastTimestampUs = 0;
};

#endif //SYNCINE_VIDEOTRACKSOURCE_H
