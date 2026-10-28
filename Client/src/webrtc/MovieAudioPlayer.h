//
// 接收端电影声的播放器：Media 连接所用假声卡的替代品。
//
// media 连接使用假声卡（kDummyAudio），既不采集也不播放
// （audio_device_dummy.cc 的 InitPlayout 与 StartPlayout 均返回 -1），这是有意为之。
// 代价是 WebRTC 解码出远端电影声后无设备可放，故这一段自行接管：
//   WebRTC 解码后的 PCM ──OnData()──> [缓冲] ──readData()──> QAudioSink ──> 扬声器
// 语音轨不需要该机制，它走真实声卡由 WebRTC 自行播放。
//
// 必须使用拉模式：OnData() 由 WebRTC 音频线程调用，而 QAudioSink 的内部缓冲在其
// 自身线程上访问，写入推模式的设备会冲突；拉模式下只需保证 readData() 线程安全。
// readData() 无数据时返回静音而非 0 字节：QAudioSink 因而永不欠载、始终按 48kHz
// 稳定拉取，成为时钟源；网络缓慢时表现为安静，而非播放器卡住或状态异常。
//
// 音量由 QAudioSink::setVolume() 控制，与语音走 WebRTC SetVolume 的路径无关。
// 输出设备同样不归 WebRTC 管理，需自行选择：默认跟随系统，用户在设置中选择听筒后
// 由 WebrtcManager 调用 setOutputDevice() 切换——语音与电影声同属「我听别人」的声音，
// 分为两个设备会导致语音在耳机、电影声在外放。
//

#ifndef SYNCINE_MOVIEAUDIOPLAYER_H
#define SYNCINE_MOVIEAUDIOPLAYER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QAudioDevice>
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

    // 切换输出设备。传入 isNull() 的设备即回到系统默认。
    // 会停止并重建 QAudioSink（设备只能在构造 QAudioSink 时指定），音量保留。
    // 只在 Qt 主线程调用。
    void setOutputDevice(const QAudioDevice &device);

    // 0.0 ~ 1.0，1.0 为原始音量
    void setVolume(double volume);
    double volume() const;

    // 已写入缓冲的字节数与当前缓冲深度（排查用）
    int queuedBytes() const;
    int droppedForLag() const;

    // 从 WebRTC 交出这段音频，到扬声器真正响出，大约还要多久（毫秒）。
    // 只在 GUI 线程调用（内部读 QAudioSink::bufferSize）。
    int playoutDelayMs() const;

    // 丢弃本地环形缓冲和一小段仍在 NetEq 里的旧音频。
    // 远端 seek 后调用；默认不 reset QAudioSink（避免一次可闻的停顿）。
    void flush();

    // ---- webrtc::AudioTrackSinkInterface ----
    // 由 WebRTC 的音频线程调用
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

    // 输出设备。默认构造为空设备 = 使用系统默认（见 startSink）
    QAudioDevice m_outputDevice;

    // 诊断：前几帧 + 每隔一段记录一条，避免淹没日志
    int m_frameCount = 0;
    // 因采样率/位深不符而被丢弃的帧数
    int m_droppedCount = 0;
    // 单声道帧数。只用于「第一次遇到时提示一句」，不参与丢弃判断。
    int m_monoCount = 0;

    // 单声道铺成立体声用的暂存区。只在 OnData 里用，而 OnData 由 WebRTC 的
    // 音频线程串行调用，故无需加锁；做成成员是为了不在音频线程上反复分配。
    std::vector<int16_t> m_monoScratch;

    // 最近一次收到音频的时间（steady clock 微秒）。拉模式下 QAudioSink 会一直
    // 保持 Active（readData 永远补静音），不能用 sink 状态判断“还有没有电影声”，
    // 只能看这个时间戳。
    std::atomic<int64_t> m_lastOnDataUs{0};

    // seek 后的一小段丢弃窗口：本地环形缓冲清掉后，NetEq 还会继续吐旧音频。
    std::atomic<int64_t> m_discardUntilUs{0};
};

#endif //SYNCINE_MOVIEAUDIOPLAYER_H
