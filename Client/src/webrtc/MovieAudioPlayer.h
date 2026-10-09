//
// 接收端电影声的播放器 —— Media 连接那条假声卡的替代品。
//
// 【为什么需要它】
//
// media 连接用的是一块"假声卡"(kDummyAudio),它既不采集也不能播放
// (见 modules/audio_device/dummy/audio_device_dummy.cc:InitPlayout 与
//  StartPlayout 都直接返回 -1)。这是故意的 —— 我们要的就是它不采集。
//
// 但代价随之而来:WebRTC 把远端电影声解码出来之后,没有设备可以放。
// 所以这一段必须我们自己接:
//
//    WebRTC 解码后的 PCM ──OnData()──> [环形缓冲] ──readData()──> QAudioSink ──> 扬声器
//
// 而语音那条不需要这套东西 —— 它走真声卡,WebRTC 自己会放。
//
// 【为什么用"拉模式"】
//
// QAudioSink 有两种用法:start() 返回一个可写的 QIODevice(推模式),
// 或 start(device) 让它从我们给的 QIODevice 里读(拉模式)。
//
// 这里必须用拉模式:OnData() 是 WebRTC 的音频线程调的,而 QAudioSink
// 的内部缓冲在自己的线程上访问 —— 直接往推模式的设备里写会撞车。
// 拉模式下我们只需要保证 readData() 是线程安全的,方向单一。
//
// 【缓冲耗尽时补静音,而不是返回 0】
//
// readData() 在没数据时返回静音而不是 0 字节。这样 QAudioSink 永远不会
// 欠载、始终按 48kHz 的节奏稳定地拉数据 —— 它就成了我们的时钟源。
// 网络慢时表现为"安静",而不是播放器卡住或状态乱跳。
//
// 【音量】
//
// 由 QAudioSink::setVolume() 控制。这是"接收端电影音量独立可调"的落点,
// 和语音那条走 WebRTC 的 SetVolume 完全无关,所以两者天然互不影响。
//

#ifndef SYNCINE_MOVIEAUDIOPLAYER_H
#define SYNCINE_MOVIEAUDIOPLAYER_H

#include <cstdint>
#include <memory>
#include <optional>

#include <QObject>

#include "api/media_stream_interface.h"
#include "api/rtp_packet_infos.h"

class QAudioSink;
class MovieAudioDevice;

class MovieAudioPlayer : public QObject,
                         public webrtc::AudioTrackSinkInterface {
    Q_OBJECT

public:
    explicit MovieAudioPlayer(QObject *parent = nullptr);
    ~MovieAudioPlayer() override;

    // 0.0 ~ 1.0,1.0 为原始音量。
    void setVolume(double volume);
    double volume() const;

    // 已写入缓冲的字节数与当前缓冲深度(排障用)
    int queuedBytes() const;

    // ---- webrtc::AudioTrackSinkInterface ----
    // 在 WebRTC 的音频线程被调用
    void OnData(const void *audio_data,
                int bits_per_sample,
                int sample_rate,
                size_t number_of_channels,
                size_t number_of_frames,
                std::optional<int64_t> absolute_capture_timestamp_ms,
                const webrtc::RtpPacketInfos &packet_infos) override;

signals:
    void volumeChanged(double volume);

private:
    void startSink();

    std::unique_ptr<MovieAudioDevice> m_device;
    std::unique_ptr<QAudioSink> m_sink;

    // 诊断:前几次 + 每隔一段打一条,避免淹没日志
    int m_frameCount = 0;
    // 因为格式或通道数不对而被丢弃的帧数
    int m_droppedCount = 0;
};

#endif //SYNCINE_MOVIEAUDIOPLAYER_H
