//
// 电影音频音源：把播放器 PCM 手工喂给 WebRTC 的 AudioSourceInterface。
//
// WebRTC 的音轨本身不产生声音，真正的数据来源是音源。麦克风轨的音源由
// CreateAudioSource() 提供，背后接系统声卡；电影音频无硬件来源，必须自行实现
// 音源并手动喂入 PCM。
//
// 帧长由本类切：WebRTC ACM 只接收恰好 10ms 的块（480 采样 @48kHz），
// 否则整帧丢弃；上游 QAudioBufferOutput 的帧长由解码器决定（AAC 一帧 1024 采样
// ≈21.3ms）。pushPcm 会攒帧切分后再扇出，不要绕过它直接调 OnData。
//
// m_sinks 加锁：AddSink/RemoveSink 来自 WebRTC 信令线程，pushPcm 来自 Qt 主线程。
//
// 本链路不经过 CreateAudioSource() 的 AudioOptions，故天然没有 AEC/AGC/NS——
// 这是所需行为（回声消除会削掉音乐，噪声抑制会滤掉配乐高频），不要为其添加这些选项。
//

#ifndef SYNCINE_MOVIEAUDIOSOURCE_H
#define SYNCINE_MOVIEAUDIOSOURCE_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "api/media_stream_interface.h"
#include "api/notifier.h"
#include "rtc_base/synchronization/mutex.h"

class MovieAudioSource : public webrtc::Notifier<webrtc::AudioSourceInterface> {
public:
    MovieAudioSource() = default;

    // ---- AudioSourceInterface ----

    // WebRTC 挂入/摘除其发送适配器
    void AddSink(webrtc::AudioTrackSinkInterface *sink) override;
    void RemoveSink(webrtc::AudioTrackSinkInterface *sink) override;

    // 本端不需要：音量在接收端通过 SetVolume 调整（见 PeerLink 的收端处理）。
    // 若在此处做增益，推送端调整音量会连带改变所有观众听到的大小。
    void SetVolume(double volume) override {}

    void RegisterAudioObserver(AudioObserver *observer) override {}
    void UnregisterAudioObserver(AudioObserver *observer) override {}

    // ---- MediaSourceInterface ----
    // RegisterObserver/UnregisterObserver 由基类 Notifier 实现，此处无需重复定义。

    SourceState state() const override {
        return kLive;
    }

    bool remote() const override {
        return false;
    }

    // ---- 本类的真正入口 ----

    // 推一帧交错（interleaved）int16 PCM 给所有已挂载的 sink。
    //
    //   data              交错排布的样本，长度 = samplesPerChannel * channels
    //   samplesPerChannel 每声道的采样数（注意不是样本总数）
    //   sampleRate        采样率，本项目约定为 48000（WebRTC 的 Opus 原生采样率）
    //   channels          声道数
    //
    // 帧长可任意：内部会切成 10ms 块（480 采样 @48kHz）后扇出，
    // 不足一块的余量留在缓冲中等下一帧补齐（见文件头「10ms 契约」）。
    // 无任何 sink 时（尚未建连 / 对端均已断开）为空操作。
    void pushPcm(const int16_t *data, size_t samplesPerChannel,
                 int sampleRate, size_t channels);

protected:
    // 受保护的析构：引用计数归零时由 scoped_refptr 释放，不允许在栈上或
    // 通过 delete 直接销毁。这是 WebRTC 引用计数类的惯例。
    ~MovieAudioSource() override = default;

private:
    webrtc::Mutex m_sinkLock;
    std::vector<webrtc::AudioTrackSinkInterface *> m_sinks RTC_GUARDED_BY(m_sinkLock);

    // 切 10ms 块用的攒帧缓冲：上游帧长（如 1024 采样）通常不是 10ms 的整数倍，
    // 切分后的余量存于此，与下一帧拼接后继续切分。
    std::vector<int16_t> m_pending RTC_GUARDED_BY(m_sinkLock);
    int m_pendingSampleRate RTC_GUARDED_BY(m_sinkLock) = 0;
    size_t m_pendingChannels RTC_GUARDED_BY(m_sinkLock) = 0;
};

#endif //SYNCINE_MOVIEAUDIOSOURCE_H
