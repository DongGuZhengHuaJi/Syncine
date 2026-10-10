//
// Created by donggu on 2026/9/28.
//

#include "MovieAudioSource.h"

#include <algorithm>
#include <optional>

#include "core/Log.h"

#include "api/rtp_packet_infos.h"

void MovieAudioSource::AddSink(webrtc::AudioTrackSinkInterface *sink) {
    if (sink == nullptr)
        return;

    webrtc::MutexLock lock(&m_sinkLock);
    m_sinks.push_back(sink);

    // 这里有日志很重要:WebRTC 是在 AddTrack 时把自己的发送适配器挂进来的。
    // 如果这行**从未出现**,说明挂载没发生 —— 那么 pushPcm 会扇出给一个空表,
    // 数据就此消失(对端只会收到 WebRTC 自己产生的静音)。
    LOG_INFO("MovieAudio") << "WebRTC 已挂入发送适配器" << static_cast<void *>(sink)
                           << "(当前" << m_sinks.size() << "个)";
}

void MovieAudioSource::RemoveSink(webrtc::AudioTrackSinkInterface *sink) {
    if (sink == nullptr)
        return;

    webrtc::MutexLock lock(&m_sinkLock);

    // 用 remove/erase 而不是 find/erase:万一同一个 sink 意外挂进来两次,
    // 这里要一次清干净,不能留下野指针 —— AddTrack 时 WebRTC 会先
    // RemoveSink 再 AddSink(见 pc/rtp_sender.cc),漏删就会重复喂数据。
    m_sinks.erase(std::remove(m_sinks.begin(), m_sinks.end(), sink),
                  m_sinks.end());
}

void MovieAudioSource::pushPcm(const int16_t *data, size_t samplesPerChannel,
                               int sampleRate, size_t channels) {
    // 参数自检。调用方(WebrtcManager)已经把过关,这里是纵深防御:
    // 这个函数每 10~20ms 就被调一次,传进垃圾值的代价是 WebRTC 内部崩,
    // 而崩溃点离这里很远,排查成本极高。
    if (data == nullptr || samplesPerChannel == 0 || channels == 0 || sampleRate <= 0)
        return;

    // WebRTC 只收"恰好 10ms"的块,块长用 ACM 自己的公式算:
    // sample_rate / 100(见 modules/audio_coding/acm2/audio_coding_module.cc
    // 的 Add10MsDataInternal,不满足就整帧丢弃)。采样率低于 100Hz 时
    // 这个公式得 0,WebRTC 也收不了,直接防御掉。
    const size_t samplesPerBlock = static_cast<size_t>(sampleRate / 100);
    if (samplesPerBlock == 0)
        return;

    webrtc::MutexLock lock(&m_sinkLock);

    // 还没建连接,或所有对端都断了 —— 正常情况,不是错误。
    // 顺手清掉攒帧缓冲:断连期间旧数据攒下来,重连后放出去反而是杂音。
    if (m_sinks.empty()) {
        m_pending.clear();
        m_pendingSampleRate = 0;
        m_pendingChannels = 0;
        return;
    }

    // 电影音频不是从网络上收来的,没有 RTP 包信息,也不需要"绝对采集时间戳":
    // 传 nullopt 让 WebRTC 按数据到达时间打点即可。接收端两条音轨之间的
    // 音画同步由 RTCP 的 sender report 负责,不依赖这个字段。
    // 诊断:每隔一段打一次"扇出给了几个 sink"和"推出去的数据峰值"。
    // 前者为 0 说明 WebRTC 的适配器没挂上来;后者为 0 说明上游给的就是静音。
    static int pushCount = 0;
    if (pushCount < 3 || pushCount % 500 == 0) {
        int peak = 0;
        const size_t total = samplesPerChannel * channels;
        for (size_t i = 0; i < total; ++i) {
            const int v = data[i] < 0 ? -data[i] : data[i];
            if (v > peak)
                peak = v;
        }
        LOG_TRACE("MovieAudio") << "第" << (pushCount + 1) << "次推送: 扇出给"
                                << m_sinks.size() << "个 sink, 峰值" << peak;
    }
    ++pushCount;

    // 格式变了(或首次进来)就清掉旧缓冲重来:半个旧格式的块和新数据拼不上。
    if (m_pendingSampleRate != sampleRate || m_pendingChannels != channels) {
        m_pending.clear();
        m_pendingSampleRate = sampleRate;
        m_pendingChannels = channels;
    }

    m_pending.insert(m_pending.end(), data, data + samplesPerChannel * channels);

    // 攒够一个 10ms 块就扇出一块,剩下的留给下一帧来拼。
    const webrtc::RtpPacketInfos emptyPacketInfos;
    while (m_pending.size() >= samplesPerBlock * channels) {
        for (webrtc::AudioTrackSinkInterface *sink : m_sinks) {
            // 注意第 5 个参数是"每声道采样数",不是样本总数。
            // 第 6 个参数用 7 参数重载而非 5 参数的那个 ——
            // WebRTC 的 LocalAudioSinkAdapter 重写的是带绝对时间戳的版本
            // (见 pc/rtp_sender.h),走 5 参数会绕过它的时间戳处理。
            sink->OnData(m_pending.data(),
                         /*bits_per_sample=*/16,
                         sampleRate,
                         channels,
                         samplesPerBlock,
                         /*absolute_capture_timestamp_ms=*/std::nullopt,
                         emptyPacketInfos);
        }
        m_pending.erase(m_pending.begin(),
                        m_pending.begin()
                            + static_cast<std::ptrdiff_t>(samplesPerBlock * channels));
    }
}
