//
// Created by donggu on 2026/10/03.
//

#include "MovieAudioPlayer.h"

#include <chrono>
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

// 电影音频的格式约定。采样率与位深必须和发送端 PlaybackController 里
// QAudioBufferOutput 构造时指定的格式一致 —— 那是这条链路的源头。
//
// 声道数不等于源头：源头给的是立体声，但对端解码出几个声道由 opus 协商的
// stereo 参数决定（见 OnData 里的说明），单声道同样要能放。这里的 kChannels
// 是**放音**侧的声道数，那由 QAudioSink 决定，设备普遍只接受立体声。
constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;

// QAudioSink 自己的缓冲深度。调小能降低延迟，调大能抗抖动。
// 100ms 是个折中：局域网下延迟可接受，也容得下常见的网络抖动。
constexpr int kSinkBufferMs = 100;

// 允许在本端队列中积压的上限。
//
// 超过该值说明网络送入的比播出的快（或 WebRTC 一次性推入一批），
// 此时必须丢弃最老的数据——宁可丢失少量声音，也不能让延迟持续累积，
// 否则观众端画面与声音的偏差会不断增大。
constexpr int kMaxQueueMs = 400;
constexpr int kMaxQueueBytes = kSampleRate * kChannels * 2 * kMaxQueueMs / 1000;

// 环形缓冲与 NetEq 丢弃窗口共用单调时钟，避免系统时间跳变。
int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

// ============================================================================
// 拉模式下的数据源头：一个线程安全的环形缓冲
// ============================================================================

// 一个"周期"的字节数。QAudioSink 拉数据是按周期来的，这里取 10ms ——
// 和 WebRTC 交付音频的粒度一致。48k × 2 声道 × 2 字节 × 10ms = 1920 字节。
constexpr int kBytesPerPeriod = kSampleRate * kChannels * 2 / 100;

class MovieAudioDevice : public QIODevice {
public:
    MovieAudioDevice() {
        // 必须真的 open() 而不是只设 openMode —— QAudioSink 会据此判断设备可用
        open(QIODevice::ReadOnly);
    }

    // 这是个只能顺序读的流，不是可随机访问的缓冲区。
    //
    // 必须声明：QIODevice 默认按"可随机访问"处理，会去调 size()/pos()/seek(),
    // 而那些对音频流没有意义 —— 拉模式下的行为会因此变得不可预期。
    bool isSequential() const override {
        return true;
    }

    // 由 WebRTC 的音频线程调用
    void push(const char *data, int bytes) {
        QMutexLocker locker(&m_mutex);

        if (bytes <= 0)
            return;

        // 单次数据比整个环形缓冲还大：只保留尾部，前面全部算丢弃。
        if (bytes > kMaxQueueBytes) {
            const int skip = bytes - kMaxQueueBytes;
            m_droppedForLag += m_size + skip;
            data += skip;
            bytes = kMaxQueueBytes;
            m_read = 0;
            m_size = 0;
        }

        // 积压过多 → 丢掉最老的一段，把延迟压回去
        if (m_size + bytes > kMaxQueueBytes) {
            const int excess = m_size + bytes - kMaxQueueBytes;
            m_read = (m_read + excess) % kMaxQueueBytes;
            m_size -= excess;
            m_droppedForLag += excess;
        }

        const int write = (m_read + m_size) % kMaxQueueBytes;
        const int firstPart = qMin(bytes, kMaxQueueBytes - write);
        std::memcpy(m_buffer.data() + write, data, static_cast<size_t>(firstPart));
        if (bytes > firstPart) {
            std::memcpy(m_buffer.data(), data + firstPart,
                        static_cast<size_t>(bytes - firstPart));
        }
        m_size += bytes;
    }

    qint64 bytesAvailable() const override {
        QMutexLocker locker(&m_mutex);
        // 关键：永远报"至少有一个周期",让 QAudioSink 觉得一直有数据可拉。
        //
        // 否则缓冲为空时 QAudioSink 会进入欠载状态并停止拉取，直到数据送达才恢复——
        // 那样播放节奏将由网络决定，抖动会直接反映到声音上。
        // 报一个整周期的量（而不是 1 字节）是为了让 QAudioSink 按正常的块大小
        // 来拉；真没数据时 readData 会用静音补满。
        return qMax<qint64>(m_size, kBytesPerPeriod);
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override {
        QMutexLocker locker(&m_mutex);

        const qint64 available = qMin<qint64>(m_size, maxSize);
        if (available > 0) {
            const int firstPart = qMin<int>(static_cast<int>(available),
                                            kMaxQueueBytes - m_read);
            std::memcpy(data, m_buffer.data() + m_read,
                        static_cast<size_t>(firstPart));
            if (available > firstPart) {
                std::memcpy(data + firstPart, m_buffer.data(),
                            static_cast<size_t>(available - firstPart));
            }
            m_read = (m_read + static_cast<int>(available)) % kMaxQueueBytes;
            m_size -= static_cast<int>(available);
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
        return m_size;
    }

    int droppedForLag() const {
        QMutexLocker locker(&m_mutex);
        return m_droppedForLag;
    }

    void clear() {
        QMutexLocker locker(&m_mutex);
        m_read = 0;
        m_size = 0;
    }

private:
    mutable QMutex m_mutex;
    // 固定大小的环形缓冲，避免 QByteArray::remove(0,n) 每次搬移整块内存。
    std::vector<char> m_buffer = std::vector<char>(kMaxQueueBytes);
    int m_read = 0;
    int m_size = 0;
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

    // 用户选了"听筒"就用它，没选（m_outputDevice 为空）就跟系统默认
    QAudioDevice device = m_outputDevice.isNull() ? QMediaDevices::defaultAudioOutput()
                                                  : m_outputDevice;
    if (device.isNull()) {
        LOG_WARN("MovieAudio") << "没有可用的音频输出设备,电影声将无处播放";
        return;
    }

    if (!device.isFormatSupported(format)) {
        // 不是致命错误：QAudioSink 会自己转。但采样率/声道对不上时
        // 音调和速度会不对，所以还是喊一声。
        LOG_WARN("MovieAudio") << "输出设备不直接支持 48kHz 双声道,交由 Qt 转换(可能有细微音质损失)";
    }

    m_sink = std::make_unique<QAudioSink>(device, format, this);
    m_sink->setBufferSize(format.bytesForDuration(kSinkBufferMs * 1000));

    // 拉模式：QAudioSink 从 m_device 里读。
    // (Qt 6 的这个重载返回 void,失败是静默的 —— 队列照样有数据但没声音，
    //  所以下面把状态和错误码打出来，启动失败时能一眼看出来。)
    m_sink->start(m_device.get());

    LOG_INFO("MovieAudio") << "已启动:"
             << kSampleRate << "Hz" << kChannels << "声道, 缓冲"
             << kSinkBufferMs << "ms, 设备:" << device.description()
             << "音量" << m_sink->volume()
             << "状态" << static_cast<int>(m_sink->state()) // 状态0正常
             << "错误" << static_cast<int>(m_sink->error());
}

void MovieAudioPlayer::setOutputDevice(const QAudioDevice &device) {
    // 设备只能在构造 QAudioSink 时指定，所以换设备 = 停掉重开一个。
    // id() 是比较设备的标准做法（description 可能重名）。
    if (device.id() == m_outputDevice.id())
        return;

    const double previousVolume = volume();

    m_outputDevice = device;

    if (m_sink != nullptr) {
        // 先停再析构：析构里也会停，但显式停一下能让错误码在日志里看得见
        m_sink->stop();
        m_sink.reset();
    }

    startSink();

    // 新 sink 的音量是默认的 1.0,把它补回用户设过的值
    if (m_sink != nullptr)
        m_sink->setVolume(previousVolume);

    LOG_INFO("MovieAudio") << "输出设备已切换:"
                           << (m_outputDevice.isNull() ? QStringLiteral("(系统默认)")
                                                       : m_outputDevice.description());
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

int MovieAudioPlayer::droppedForLag() const {
    return m_device != nullptr ? m_device->droppedForLag() : 0;
}

int MovieAudioPlayer::playoutDelayMs() const {
    // 拉模式下 sink 永远 Active（readData 补静音），没有音频时不能用 state 判断。
    // 最近 500ms 没有 OnData 就当“没有电影声”，返回 0；恢复播放后的最初 500ms
    // 由渲染器那边保持上一次的延迟值，不会瞬间失配。
    const int64_t lastOnDataUs = m_lastOnDataUs.load(std::memory_order_relaxed);
    if (lastOnDataUs == 0 || steadyNowUs() - lastOnDataUs > 500 * 1000)
        return 0;

    constexpr int kBytesPerMs = kSampleRate * kChannels * 2 / 1000; // 192
    const int queued = m_device != nullptr ? m_device->queuedBytes() : 0;
    // bufferSize() 是 sink 的整个缓冲；拉模式下实际占用可能略少，这里取保守上界。
    const int sinkBytes = m_sink != nullptr
                              ? static_cast<int>(m_sink->bufferSize())
                              : 0;
    int delay = (queued + sinkBytes) / kBytesPerMs;

    // 声卡/驱动的固定延迟，默认 0；需要校准时用环境变量补一个常数。
    static const int extraMs = qMax(0, qEnvironmentVariableIntValue("SYNCINE_AUDIO_LATENCY_MS"));
    delay += extraMs;
    return delay;
}

void MovieAudioPlayer::flush() {
    if (m_device != nullptr)
        m_device->clear();

    // NetEq 里已经解出的旧音频还会继续从 OnData 到达；给一个小丢弃窗口，
    // 避免 seek 后先听到 100~300ms 的旧声音。
    m_discardUntilUs.store(steadyNowUs() + 250 * 1000, std::memory_order_relaxed);
    LOG_DEBUG("MovieAudio") << "已清空电影声缓冲(含 250ms NetEq 丢弃窗口)";
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

    const int64_t nowUs = steadyNowUs();
    m_lastOnDataUs.store(nowUs, std::memory_order_relaxed);

    // seek 后的一小段窗口：本地缓冲已经清了，但 NetEq 还在吐旧数据。
    if (nowUs < m_discardUntilUs.load(std::memory_order_relaxed))
        return;

    // 采样率与位深不符合预期就丢掉。不在这里做重采样：发送端已经用
    // QAudioBufferOutput 把这两项定死成 48k/Int16,真出现别的值说明链路上游
    // 出了问题，应该去修上游而不是在这里兼容。
    //
    // 声道数则**只接受 1 或 2**，不做丢弃判断 —— 它不是发送端定的，而是协商出来的：
    // 远端那条 opus 的 SDP 里带 stereo=1 才是双声道，否则 NetEq 按单声道解码
    // （解码器的声道数取自 stereo 参数，缺省为 1：audio_decoder_opus.cc 的
    // GetDefaultNumChannels；neteq_impl.cc:430 又把输出声道数取自解码器）。
    // 协商成单声道时整条链路是自洽的（发送端 ACM 也会把立体声下混成单声道），
    // 这里若照着双声道丢弃，结果就是全程静音。
    if (bits_per_sample != 16 || sample_rate != kSampleRate
        || (number_of_channels != 1 && number_of_channels != kChannels)) {
        if (m_droppedCount == 0) {
            LOG_WARN("MovieAudio") << "收到的音频格式不符合预期,已丢弃:"
                       << bits_per_sample << "bit" << sample_rate << "Hz"
                       << number_of_channels << "声道";
        }
        ++m_droppedCount;
        return;
    }

    if (number_of_channels == 1) {
        // 单声道 → 双声道：每个样本复制一份给左右。QAudioSink 那份格式是立体声
        // （设备普遍不接受单声道格式），不铺开就等于不出声。
        if (m_monoCount == 0)
            LOG_INFO("MovieAudio") << "远端电影声为单声道(opus 未协商 stereo),已铺成立体声播放";
        ++m_monoCount;

        const auto *samples = static_cast<const int16_t *>(audio_data);
        m_monoScratch.resize(number_of_frames * 2);
        for (size_t i = 0; i < number_of_frames; ++i) {
            m_monoScratch[i * 2] = samples[i];
            m_monoScratch[i * 2 + 1] = samples[i];
        }
        m_device->push(reinterpret_cast<const char *>(m_monoScratch.data()),
                       static_cast<int>(m_monoScratch.size() * sizeof(int16_t)));
    } else {
        const int bytes = static_cast<int>(number_of_frames * number_of_channels * 2);
        m_device->push(static_cast<const char *>(audio_data), bytes);
    }

    // 诊断：开头几次 + 每隔一段。
    //
    // **峰值**是这里最关键的一项，它能把问题一刀切成两半：
    //   峰值恒为 0  → 传过来的是静音，问题在发送端/传输链路
    //   峰值非 0    → 数据是对的，问题在播放设备这一段（音量/设备/状态）
    if (m_frameCount < 3 || m_frameCount % 500 == 0) {
        const auto *samples = static_cast<const int16_t *>(audio_data);
        const size_t total = number_of_frames * number_of_channels;
        int peak = 0;
        for (size_t i = 0; i < total; ++i) {
            const int v = samples[i] < 0 ? -samples[i] : samples[i];
            if (v > peak)
                peak = v;
        }

        // 只在前几帧报告静音。发送端停发后，接收端仍会从 NetEq 持续拿到
        // 全 0 帧（muted state），每 5 秒一行的静音日志没有信息量。
        if (m_frameCount < 3 || peak != 0) {
            // **不要在这里读 m_sink**：OnData 在 WebRTC 音频线程，而
            // setOutputDevice() 会在 GUI 线程 reset 它。队列/丢帧都由 device 的
            // 互斥量保护，足够定位问题。
            LOG_TRACE("MovieAudio") << "第" << (m_frameCount + 1) << "帧: 峰值" << peak
                     << "队列积压" << m_device->queuedBytes() << "字节"
                     << "丢字节" << m_device->droppedForLag();
        }
    }
    ++m_frameCount;
}
