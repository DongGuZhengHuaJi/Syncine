//
// 视频轨道的"源" —— 帧的入口。
//
// 为什么需要它:
//
//   WebRTC 音频侧有现成的 ADM(麦克风采集),视频侧却没有"从别处拿帧"的实现 ——
//   WebRTC 只给接口,数据必须自己喂。这个类就是那个喂入口。
//
//   Qt 解码帧 ──(格式转换)──→ 本类 ──→ 编码器 ──→ RTP
//        将来接                        ↑
//                                  VideoBroadcaster 负责分发
//
// 当前阶段:只做"接收并广播",**不做格式转换** —— 调用方直接给
// webrtc::VideoFrame。这样能先把链路验证通,转换那一层等链路稳了再加,
// 出问题时才分得清是链路错还是转换错。
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


    // 喂一帧 WebRTC 格式的帧并广播给所有订阅者。
    void pushFrame(const webrtc::VideoFrame &frame);

    // 喂一帧 Qt 格式的帧。由 PlaybackController 的 videoFrameAvailable 信号驱动(main.cpp 里接线)。
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

    // 生命周期由 WebrtcManager 掌握,Release 永远返回 kOtherRefsRemained
    void AddRef() const override {
        ++m_refCount;
    }

    webrtc::RefCountReleaseStatus Release() const override {
        --m_refCount;
        return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
    }

private:
    webrtc::VideoBroadcaster m_broadcaster;

    // 可变的,因为 AddRef/Release 是 const 方法
    mutable std::atomic<int> m_refCount{0};

    // VideoBroadcaster 不暴露订阅者数量,自己记一个。
    // 按指针去重:同一根轨道重复注册时 AddOrUpdateSink 也会被调用。
    int m_sinkCount = 0;
    std::set<webrtc::VideoSinkInterface<webrtc::VideoFrame> *> m_registeredSinks;

    int m_width = 0;
    int m_height = 0;
    bool m_hasReceivedFrame = false;

    // 上一帧的单调时间戳,用来保证严格递增
    int64_t m_lastTimestampUs = 0;
};

#endif //SYNCINE_VIDEOTRACKSOURCE_H
