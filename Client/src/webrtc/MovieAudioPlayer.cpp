//
// Created by donggu on 2026/10/03.
//

#include "MovieAudioPlayer.h"

#include <cstring>

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>

#include "core/Log.h"

namespace {

// 电影音频的格式约定。和发送端 PlaybackController 里 QAudioBufferOutput
// 构造时指定的格式必须一致 —— 那是这条链路的源头。
// 双声道:发送端给的立体声 PCM 会被按 2 声道编码(前提是 media 工厂的
// 编解码器工厂都加了 stereo=1,见 WebrtcManager.cpp),这里按 2 声道放。
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;

// QAudioSink 自己的缓冲深度。调小能降低延迟,调大能抗抖动。
// 100ms 是个折中:局域网下延迟可接受,也容得下常见的网络抖动。
constexpr int kSinkBufferMs = 100;

// 允许在"我们自己的队列"里积压的上限。
//
// 超过它就说明网络送来的比播出去的快(或者 WebRTC 那边一下子推了一批),
// 这时候必须丢**最老的**数据 —— 宁可丢一点声音,也不能让延迟越攒越大,
// 那样观众会看到画面和声音越差越远。
constexpr int kMaxQueueMs = 400;

} // namespace

// ============================================================================
// 拉模式下的数据源头:一个线程安全的环形缓冲
// ============================================================================

// 一个"周期"的字节数。QAudioSink 拉数据是按周期来的,这里取 10ms ——
// 和 WebRTC 交付音频的粒度一致。
constexpr int kBytesPerPeriod = kSampleRate * kChannels * 2 / 100;   // 960

class MovieAudioDevice : public QIODevice {
public:
    MovieAudioDevice() {
        // 必须真的 open() 而不是只设 openMode —— QAudioSink 会据此判断设备可用
        open(QIODevice::ReadOnly);
    }

    // 这是个只能顺序读的流,不是可随机访问的缓冲区。
    //
    // 必须声明:QIODevice 默认按"可随机访问"处理,会去调 size()/pos()/seek(),
    // 而那些对音频流没有意义 —— 拉模式下的行为会因此变得不可预期。
    bool isSequential() const override {
        return true;
    }

    // 由 WebRTC 的音频线程调用
    void push(const char *data, int bytes) {
        QMutexLocker locker(&m_mutex);

        m_buffer.append(data, bytes);

        // 积压过多 → 丢掉最老的一段,把延迟压回去
        const int maxBytes = kSampleRate * kChannels * 2 * kMaxQueueMs / 1000;
        if (m_buffer.size() > maxBytes) {
            const int excess = m_buffer.size() - maxBytes;
            m_buffer.remove(0, excess);
            m_droppedForLag += excess;
        }

        m_totalWritten += bytes;
    }

    qint64 bytesAvailable() const override {
        QMutexLocker locker(&m_mutex);
        // 关键:永远报"至少有一个周期",让 QAudioSink 觉得一直有数据可拉。
        //
        // 否则缓冲空的时候 QAudioSink 会进入欠载状态、停止拉取,等我们把
        // 数据送到才恢复 —— 那样播放节奏就由网络决定,抖动会直接传到声音上。
        // 报一个整周期的量(而不是 1 字节)是为了让 QAudioSink 按正常的块大小
        // 来拉;真没数据时 readData 会用静音补满。
        return qMax<qint64>(m_buffer.size(), kBytesPerPeriod);
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override {
        QMutexLocker locker(&m_mutex);

        const qint64 available = qMin<qint64>(m_buffer.size(), maxSize);
        if (available > 0) {
            std::memcpy(data, m_buffer.constData(), static_cast<size_t>(available));
            m_buffer.remove(0, static_cast<int>(available));
        }

        // 剩下的补静音 —— 见头文件里"缓冲耗尽时补静音"的说明
        if (available < maxSize) {
            std::memset(data + available, 0, static_cast<size_t>(maxSize - available));
        }

        return maxSize;
    }

    qint64 writeData(const char *, qint64) override {
        return -1;   // 只读设备
    }

public:
    int queuedBytes() const {
        QMutexLocker locker(&m_mutex);
        return static_cast<int>(m_buffer.size());
    }

    int droppedForLag() const {
        QMutexLocker locker(&m_mutex);
        return m_droppedForLag;
    }

private:
    mutable QMutex m_mutex;
    QByteArray m_buffer;
    int m_totalWritten = 0;
    int m_droppedForLag = 0;
};

// ============================================================================
// MovieAudioPlayer
// ============================================================================

MovieAudioPlayer::MovieAudioPlayer(QObject *parent)
    : QObject(parent) {
    m_device = std::make_unique<MovieAudioDevice>();
    startSink();
}

MovieAudioPlayer::~MovieAudioPlayer() = default;

void MovieAudioPlayer::startSink() {
    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(kChannels);
    format.setSampleFormat(QAudioFormat::Int16);

    QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (device.isNull()) {
        LOG_WARN("MovieAudio") << "没有可用的音频输出设备,电影声将无处播放";
        return;
    }

    if (!device.isFormatSupported(format)) {
        // 不是致命错误:QAudioSink 会自己转。但采样率/声道对不上时
        // 音调和速度会不对,所以还是喊一声。
        LOG_WARN("MovieAudio") << "输出设备不直接支持 48kHz 单声道,交由 Qt 转换(可能有细微音质损失)";
    }

    m_sink = std::make_unique<QAudioSink>(device, format, this);
    m_sink->setBufferSize(format.bytesForDuration(kSinkBufferMs * 1000));

    // 拉模式:QAudioSink 从 m_device 里读。
    // (Qt 6 的这个重载返回 void,失败是静默的 —— 队列照样有数据但没声音,
    //  所以下面把状态和错误码打出来,启动失败时能一眼看出来。)
    m_sink->start(m_device.get());

    LOG_INFO("MovieAudio") << "已启动:"
             << kSampleRate << "Hz" << kChannels << "声道, 缓冲"
             << kSinkBufferMs << "ms, 设备:" << device.description()
             << "音量" << m_sink->volume()
             << "状态" << static_cast<int>(m_sink->state()) // 状态0正常
             << "错误" << static_cast<int>(m_sink->error());
}

void MovieAudioPlayer::setVolume(double volume) {
    volume = qBound(0.0, volume, 1.0);

    if (m_sink == nullptr)
        return;

    if (qFuzzyIsNull(m_sink->volume() - volume))
        return;

    m_sink->setVolume(volume);
    emit volumeChanged(volume);
}

double MovieAudioPlayer::volume() const {
    return m_sink != nullptr ? m_sink->volume() : 1.0;
}

int MovieAudioPlayer::queuedBytes() const {
    return m_device != nullptr ? m_device->queuedBytes() : 0;
}

void MovieAudioPlayer::OnData(const void *audio_data,
                              int bits_per_sample,
                              int sample_rate,
                              size_t number_of_channels,
                              size_t number_of_frames,
                              std::optional<int64_t> absolute_capture_timestamp_ms,
                              const webrtc::RtpPacketInfos &packet_infos) {
    Q_UNUSED(absolute_capture_timestamp_ms);
    Q_UNUSED(packet_infos);

    if (audio_data == nullptr || number_of_frames == 0)
        return;

    // 格式不符合预期就丢掉。不在这里做转换:发送端已经用
    // QAudioBufferOutput 把格式定死成 48k/单声道/Int16,
    // 真出现别的格式说明链路上游出了问题,应该去修上游而不是在这里兼容。
    if (bits_per_sample != 16 || sample_rate != kSampleRate
        || number_of_channels != kChannels) {
        if (m_droppedCount == 0) {
            LOG_WARN("MovieAudio") << "收到的音频格式不符合预期,已丢弃:"
                       << bits_per_sample << "bit" << sample_rate << "Hz"
                       << number_of_channels << "声道";
        }
        ++m_droppedCount;
        return;
    }

    const int bytes = static_cast<int>(number_of_frames * number_of_channels * 2);
    m_device->push(static_cast<const char *>(audio_data), bytes);

    // 诊断:开头几次 + 每隔一段。
    //
    // **峰值**是这里最关键的一项,它能把问题一刀切成两半:
    //   峰值恒为 0  → 传过来的是静音,问题在发送端/传输链路
    //   峰值非 0    → 数据是对的,问题在播放设备这一段(音量/设备/状态)
    if (m_frameCount < 3 || m_frameCount % 500 == 0) {
        const auto *samples = static_cast<const int16_t *>(audio_data);
        const size_t total = number_of_frames * number_of_channels;
        int peak = 0;
        for (size_t i = 0; i < total; ++i) {
            const int v = samples[i] < 0 ? -samples[i] : samples[i];
            if (v > peak)
                peak = v;
        }

        LOG_TRACE("MovieAudio") << "第" << (m_frameCount + 1) << "帧: 峰值" << peak
                 << "队列积压" << m_device->queuedBytes() << "字节"
                 << "音量" << volume()
                 << "sink 状态" << (m_sink ? static_cast<int>(m_sink->state()) : -1)
                 << "sink 错误" << (m_sink ? static_cast<int>(m_sink->error()) : -1)
                 << "丢帧" << m_device->droppedForLag();
    }
    ++m_frameCount;
}
