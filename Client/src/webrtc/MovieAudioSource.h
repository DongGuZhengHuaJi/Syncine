//
// 电影音频的"音源"—— PCM 到 WebRTC 之间的适配层。
//
// 【为什么需要这个类】
//
// WebRTC 里一条音轨(AudioTrack)本身不产生声音。它是"把某个音源接到
// PeerConnection 上"的插头,真正供数据的是音源(AudioSourceInterface)。
//
// 麦克风那条音轨的音源是 CreateAudioSource() 给的,背后接着系统声卡,
// WebRTC 自己会去采集。而电影音频没有硬件来源 —— 它是我们自己从播放器里
// 取出来的 PCM,WebRTC 无从知晓。所以必须自己实现一个 AudioSourceInterface,
// 把 PCM 手动喂进去。这就是本类存在的全部理由。
//
// 【WebRTC 怎么拿到我们的数据】
//
// AddTrack 时,WebRTC 会把自己的发送适配器(LocalAudioSinkAdapter,
// 一个 AudioTrackSinkInterface)挂到音轨上,而音轨会把它转交给音源
// (见 pc/audio_track.cc:  AudioTrack::AddSink → audio_source_->AddSink)。
//
// 于是流程变成: 我们 pushPcm() → 遍历 m_sinks 喂给 LocalAudioSinkAdapter
//              → WebRTC 内部编码(Opus)、打包、发送。
// 编码和发送完全不用我们操心,本类只做"扇出"这一件事。
//
// 【一个必须知道的副作用(是好事)】
//
// 这条链路**不经过** CreateAudioSource() 的 AudioOptions,所以电影音频
// 天然没有 AEC/AGC/NS。这正是想要的:回声消除会把音乐当回声削掉、
// 噪声抑制会把配乐的高频当噪声滤掉。别顺手给这条链加上那些选项。
//
// 【10ms 契约 —— 帧长必须由这里切】
//
// WebRTC 的 ACM 只收"恰好 10ms"的块(modules/audio_coding/acm2/
// audio_coding_module.cc 的 Add10MsDataInternal 要求
// samples_per_channel == sample_rate / 100,不满足整帧丢弃)。
// 而上游 QAudioBufferOutput 的帧长由解码器决定(AAC 一帧 1024 采样
// ≈21.3ms),不是 10ms。pushPcm 里会把上游的帧攒起来切成 480 采样
// (@48kHz)再扇出 —— 别把这个切分逻辑绕过去直接调 OnData。
//
// 【线程】
//
// AddSink/RemoveSink 由 WebRTC 的信令线程调用,pushPcm 由 Qt 主线程调用,
// 所以 m_sinks 必须加锁。持锁期间调 sink->OnData 是安全的 ——
// WebRTC 自己的 RemoteAudioSource 就是这么做的(见 pc/remote_audio_source.cc)。
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

    // WebRTC 把它的发送适配器挂进来 / 摘下去。
    void AddSink(webrtc::AudioTrackSinkInterface *sink) override;
    void RemoveSink(webrtc::AudioTrackSinkInterface *sink) override;

    // 本端不需要:音量是在**接收端**用 SetVolume 调的(见 PeerLink 的收端处理)。
    // 若在这里做增益,推送端一调音量会连带改变所有观众听到的大小,不是我们要的。
    void SetVolume(double volume) override {}

    void RegisterAudioObserver(AudioObserver *observer) override {}
    void UnregisterAudioObserver(AudioObserver *observer) override {}

    // ---- MediaSourceInterface ----
    // RegisterObserver/UnregisterObserver 由基类 Notifier 实现,这里不用重复写。

    SourceState state() const override {
        return kLive;
    }

    bool remote() const override {
        return false;
    }

    // ---- 本类的真正入口 ----

    // 推一帧交错(interleaved)int16 PCM 给所有已挂上的 sink。
    //
    //   data             交错排布的样本,长度 = samplesPerChannel * channels
    //   samplesPerChannel 每声道的采样数(注意:不是样本总数)
    //   sampleRate       采样率,本项目的约定是 48000(WebRTC 的 Opus 原生采样率)
    //   channels         声道数
    //
    // 帧长可以是任意的 —— 内部会切成 10ms 块(480 采样 @48kHz)再扇出,
    // 不够一块的剩余留在缓冲里等下一帧来补齐。见头文件顶部"10ms 契约"。
    // 没有任何 sink(还没建连接 / 对端都断了)时是空操作。
    void pushPcm(const int16_t *data, size_t samplesPerChannel,
                 int sampleRate, size_t channels);

protected:
    // 受保护的析构:引用计数归零时由 scoped_refptr 释放,
    // 不允许在栈上或 delete 直接销毁。这是 WebRTC 引用计数类的惯例。
    ~MovieAudioSource() override = default;

private:
    webrtc::Mutex m_sinkLock;
    std::vector<webrtc::AudioTrackSinkInterface *> m_sinks RTC_GUARDED_BY(m_sinkLock);

    // 切 10ms 块用的攒帧缓冲:上游帧长(如 1024 采样)通常不是 10ms 的
    // 整数倍,切剩下的样本放这里,和下一帧拼上继续切。
    std::vector<int16_t> m_pending RTC_GUARDED_BY(m_sinkLock);
    int m_pendingSampleRate RTC_GUARDED_BY(m_sinkLock) = 0;
    size_t m_pendingChannels RTC_GUARDED_BY(m_sinkLock) = 0;
};

#endif //SYNCINE_MOVIEAUDIOSOURCE_H
