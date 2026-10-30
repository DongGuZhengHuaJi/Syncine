//
// 系统声音回声探针（Linux，手动运行，不进 CTest）。
//
// 目的：验证 Linux「虚拟 Sink + 流路由」是否真的把 Syncine 自己的
// 房间语音排除在系统声音采集之外，同时确认其他应用的声音仍能被采到。
//
// 结构：
//   · 主进程：真实 WebRTC ADM 播放宽带噪声（模拟房间语音，属于本进程）；
//     PulseMonitorCapture 采集（默认走虚拟 Sink 路由）；统计 1kHz 附近
//     噪声频段与 4kHz 单音的能量。
//   · 子进程（--other-tone）：用 QAudioSink 播放 4kHz 单音，扮演
//     「其他应用」。必须是独立进程：路由按 PID 排除本进程，同进程的
//     QAudioSink 会被当成自己人。
//
// 用法（需要真实音频会话）：
//   ./syncine_echo_probe                            # 默认：隔离模式
//   SYNCINE_SCREEN_AUDIO_ISOLATE=0 ./syncine_echo_probe  # 回退整机 monitor
//   ./syncine_echo_probe --selftest                 # AEC 离线自检
//
// 判定：
//   · 隔离模式：ADM 开关前后 rawVoice 不应有明显变化（房间语音被排除），
//     且 rawOther 足够高（其他应用仍被采集）；
//   · 回退模式：ADM 开关前后 rawVoice 应有明显变化（复现回声，对照成立）。
//
// 历史结论（--selftest）：WebRTC APM 的 AEC3 残余回声抑制会把无关的
// 近端声音一起压掉，因此没有采用 AEC 方案。
//
// 只用 Linux + libpulse 构建；其他平台不生成该目标。
//

#include <QCoreApplication>
#include <QMutex>
#include <QMutexLocker>
#include <QProcess>
#include <QTimer>
#include <QtGlobal>

#include <pulse/error.h>
#include <pulse/simple.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/audio/audio_device_defines.h"
#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment_factory.h"
#include "api/scoped_refptr.h"

#include "capture/PulseMonitorCapture.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;
constexpr int kFrameMs = 10;
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000; // 480
// 房间语音用宽带噪声模拟：AEC 的自适应滤波对纯音的收敛很差，
// 用固定音调会得到「AEC 无效」的假阴性。
constexpr double kOtherAppHz = 4000.0;   // 代表其他应用的声音（带外单音）

// 单频能量计：相位连续的正交解调，窗口内累积后取幅值平方。
struct ToneMeter {
    double freq = 0.0;
    double inPhase = 0.0;
    double quadrature = 0.0;
    double samples = 0.0;

    void reset()
    {
        inPhase = 0.0;
        quadrature = 0.0;
        samples = 0.0;
    }

    void process(const int16_t *data, size_t count)
    {
        const double omega = 2.0 * M_PI * freq / kSampleRate;
        for (size_t i = 0; i < count; ++i) {
            const double angle = omega * samples;
            const double sample = static_cast<double>(data[i]);
            inPhase += sample * std::cos(angle);
            quadrature += sample * std::sin(angle);
            samples += 1.0;
        }
    }

    // 与窗口长度无关的均方振幅（正弦的幅值平方近似）。
    double power() const
    {
        if (samples <= 0.0)
            return 0.0;
        const double magnitude = std::hypot(inPhase, quadrature);
        return (magnitude / samples) * (magnitude / samples) * 4.0;
    }
};

// 二阶带通（RBJ，常数峰值增益），用于测量宽带噪声在语音频段的均方值。
class BandMeter {
public:
    BandMeter()
    {
        const double w0 = 2.0 * M_PI * 1000.0 / kSampleRate;
        const double alpha = std::sin(w0) / (2.0 * 0.707);

        const double a0 = 1.0 + alpha;
        m_b0 = alpha / a0;
        m_b2 = -alpha / a0;
        m_a1 = -2.0 * std::cos(w0) / a0;
        m_a2 = (1.0 - alpha) / a0;
    }

    void reset()
    {
        m_x1 = m_x2 = m_y1 = m_y2 = 0.0;
        m_squares = 0.0;
        m_samples = 0;
    }

    void process(const int16_t *data, size_t count)
    {
        for (size_t i = 0; i < count; ++i) {
            const double x = static_cast<double>(data[i]);
            const double y = m_b0 * x + m_b2 * m_x2 - m_a1 * m_y1
                             - m_a2 * m_y2;
            m_x2 = m_x1;
            m_x1 = x;
            m_y2 = m_y1;
            m_y1 = y;
            m_squares += y * y;
            ++m_samples;
        }
    }

    double power() const
    {
        if (m_samples == 0)
            return 0.0;
        return m_squares / static_cast<double>(m_samples);
    }

private:
    double m_b0 = 0.0;
    double m_b2 = 0.0;
    double m_a1 = 0.0;
    double m_a2 = 0.0;
    double m_x1 = 0.0, m_x2 = 0.0, m_y1 = 0.0, m_y2 = 0.0;
    double m_squares = 0.0;
    uint64_t m_samples = 0;
};

double toDb(double power)
{
    return 10.0 * std::log10(power + 1e-12);
}

struct Measurement {
    BandMeter rawVoice;
    ToneMeter rawOther;
    BandMeter aecVoice;
    ToneMeter aecOther;

    Measurement()
    {
        rawOther.freq = kOtherAppHz;
        aecOther.freq = kOtherAppHz;
    }

    void reset()
    {
        rawVoice.reset();
        rawOther.reset();
        aecVoice.reset();
        aecOther.reset();
    }
};

// 让 ADM 的播放线程按真实节奏向我们拉 PCM。这就是房间语音的出路：
// ADM 播放线程 → Pulse 播放流 → sink 混音 → monitor 采集。
class ToneTransport : public webrtc::AudioTransport {
public:
    int32_t RecordedDataIsAvailable(const void *, size_t, size_t, size_t,
                                    uint32_t, uint32_t, int32_t, uint32_t,
                                    bool, uint32_t &newMicLevel) override
    {
        newMicLevel = 0;
        return 0;
    }

    int32_t NeedMorePlayData(size_t nSamples, size_t nBytesPerSample,
                             size_t nChannels, uint32_t samplesPerSec,
                             void *audioSamples, size_t &nSamplesOut,
                             int64_t *, int64_t *) override
    {
        nSamplesOut = nSamples;
        Q_UNUSED(nBytesPerSample);
        if (m_formatReported.exchange(true, std::memory_order_relaxed) == false) {
            std::fprintf(stderr,
                         "ADM 播放格式: %uHz %zu声道 %zu帧/次 %zu字节/样本\n",
                         samplesPerSec, nChannels, nSamples, nBytesPerSample);
        }

        auto *out = static_cast<int16_t *>(audioSamples);
        const bool audible = !m_muted.load(std::memory_order_relaxed);

        for (size_t frame = 0; frame < nSamples; ++frame) {
            // xorshift 白噪声：比纯音更容易让 AEC 的线性滤波器收敛。
            m_rng ^= m_rng << 13;
            m_rng ^= m_rng >> 17;
            m_rng ^= m_rng << 5;
            const int32_t noise =
                static_cast<int32_t>(m_rng >> 16) - 32768;
            const int16_t value = audible
                                      ? static_cast<int16_t>(noise * 0.35)
                                      : 0;

            // 参考信号固定按单声道保存：所有声道内容相同，取一份即可。
            pushReference(value);

            for (size_t channel = 0; channel < nChannels; ++channel)
                out[frame * nChannels + channel] = value;
        }
        return 0;
    }

    void PullRenderData(int, int, size_t, size_t, void *, int64_t *,
                        int64_t *) override
    {
    }

    void setMuted(bool muted) {
        m_muted.store(muted, std::memory_order_relaxed);
    }

    // 取 10ms 参考帧；不足时补零（采集刚启动时参考还没到）。
    void takeReference(std::vector<int16_t> &frame)
    {
        frame.assign(kFrameSamples, 0);
        QMutexLocker locker(&m_referenceMutex);
        const size_t count = std::min(frame.size(), m_reference.size());
        std::copy(m_reference.begin(), m_reference.begin() + count,
                  frame.begin());
        m_reference.erase(m_reference.begin(), m_reference.begin() + count);
    }

    size_t maxQueueDepth() const
    {
        return m_maxQueueDepth.load(std::memory_order_relaxed);
    }

private:
    void pushReference(int16_t sample)
    {
        QMutexLocker locker(&m_referenceMutex);
        // 参考队列只需保留最近一段（AEC 的延迟搜索窗口远小于 2 秒）。
        if (m_reference.size() >= static_cast<size_t>(kSampleRate * 2))
            m_reference.pop_front();
        m_reference.push_back(sample);

        const size_t depth = m_reference.size();
        size_t current = m_maxQueueDepth.load(std::memory_order_relaxed);
        while (depth > current
               && !m_maxQueueDepth.compare_exchange_weak(
                      current, depth, std::memory_order_relaxed)) {
        }
    }

    std::atomic<bool> m_muted{false};
    std::atomic<bool> m_formatReported{false};
    uint32_t m_rng = 0x12345678;
    std::atomic<size_t> m_maxQueueDepth{0};

    QMutex m_referenceMutex;
    std::deque<int16_t> m_reference;
};

// 其他应用的 3000Hz 测试音，走普通 QAudioSink（与 ADM 无关）。
struct Snapshot {
    double rawVoiceDb = 0.0;
    double rawOtherDb = 0.0;
    double aecVoiceDb = 0.0;
    double aecOtherDb = 0.0;
};

class Probe {
public:
    Probe()
    {
        m_environment.emplace(webrtc::CreateEnvironment());

        // ---- APM：只开回声消除，其他模块全关 ----
        webrtc::AudioProcessing::Config config;
        config.echo_canceller.enabled = true;
        // 系统声音不是麦克风信号，不要强制高通，否则音乐低频会被削。
        config.echo_canceller.enforce_high_pass_filtering = false;
        config.noise_suppression.enabled = false;
        config.high_pass_filter.enabled = false;
        config.gain_controller1.enabled = false;
        config.gain_controller2.enabled = false;

        webrtc::BuiltinAudioProcessingBuilder builder(config);
        m_apm = builder.Build(*m_environment);
        if (m_apm == nullptr) {
            std::fprintf(stderr, "创建 APM 失败\n");
            m_ok = false;
            return;
        }
        m_apm->Initialize();
        // APM 要求开启回声消除时设置初始延迟提示；AEC3 会在此基础上自适应。
        m_apm->set_stream_delay_ms(0);
        std::fprintf(stderr, "APM echo_canceller=%d\n",
                     m_apm->GetConfig().echo_canceller.enabled ? 1 : 0);

        // 先起采集、再起 ADM 播放：参考队列从 0 开始增长，AEC 看到的
        // 延迟只有 ADM/Pulse 的输出缓冲，不会被采集启动期间的积压带偏。
        m_capture = std::make_unique<PulseMonitorCapture>();
        if (!m_capture->start(
                QString(),
                [this](const int16_t *data, size_t samples, int rate,
                       size_t channels) { onPcm(data, samples, rate, channels); },
                [](const QString &message) {
                    std::fprintf(stderr, "采集错误: %s\n", qPrintable(message));
                })) {
            std::fprintf(stderr, "启动系统声音采集失败\n");
            m_ok = false;
            return;
        }

        // ---- WebRTC ADM（真实房间语音出口）----
        m_adm = webrtc::CreateAudioDeviceModule(
            *m_environment, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
        if (m_adm == nullptr) {
            std::fprintf(stderr, "创建 ADM 失败\n");
            m_ok = false;
            return;
        }
        m_adm->RegisterAudioCallback(&m_transport);
        const int initResult = m_adm->Init();
        // Linux 下 0 号是 Pulse 的 "default" 伪设备，会跟随系统默认 sink。
        // 不显式设置时 _outputDeviceIsSpecified 为 false，InitPlayout 直接 -1。
        const int deviceResult = initResult == 0 ? m_adm->SetPlayoutDevice(0) : -1;
        std::fprintf(stderr,
                     "ADM Init=%d PlayoutDevices=%d SetPlayoutDevice=%d "
                     "InitPlayout=",
                     initResult, m_adm->PlayoutDevices(), deviceResult);
        const int initPlayoutResult = m_adm->InitPlayout();
        std::fprintf(stderr, "%d StartPlayout=", initPlayoutResult);
        const int startResult = m_adm->StartPlayout();
        std::fprintf(stderr, "%d\n", startResult);
        if (initResult != 0 || initPlayoutResult != 0 || startResult != 0) {
            std::fprintf(stderr, "启动 ADM 播放失败\n");
            m_ok = false;
            return;
        }
    }

    ~Probe()
    {
        if (m_capture)
            m_capture->stop();
        if (m_adm != nullptr) {
            m_adm->StopPlayout();
            m_adm->Terminate();
            m_adm = nullptr;
        }
    }

    bool ok() const { return m_ok; }

    // 当前采集是否走「虚拟 Sink + 流路由」（排除本进程声音）。
    bool isolated() const
    {
        return m_capture != nullptr && m_capture->isolatesOwnAudio();
    }

    void resetMeasurement()
    {
        QMutexLocker locker(&m_measureMutex);
        m_measurement.reset();
    }

    Snapshot snapshot()
    {
        QMutexLocker locker(&m_measureMutex);
        Snapshot result;
        result.rawVoiceDb = toDb(m_measurement.rawVoice.power());
        result.rawOtherDb = toDb(m_measurement.rawOther.power());
        result.aecVoiceDb = toDb(m_measurement.aecVoice.power());
        result.aecOtherDb = toDb(m_measurement.aecOther.power());
        return result;
    }

    void setVoiceMuted(bool muted) { m_transport.setMuted(muted); }

    size_t maxReferenceQueue() const
    {
        return m_transport.maxQueueDepth();
    }

    double referenceRmsDb() const
    {
        const uint64_t count = m_referenceSamples.load(std::memory_order_relaxed);
        if (count == 0)
            return -200.0;
        return 10.0 * std::log10(
                   m_referenceSquares.load(std::memory_order_relaxed)
                       / static_cast<double>(count)
                   + 1e-9);
    }

    void printApmStats(const char *phase)
    {
        const webrtc::AudioProcessingStats stats = m_apm->GetStatistics(true);
        std::printf("phase=%s stats erl=%s erle=%s divergent=%s "
                    "residual=%s delay=%s\n",
                    phase,
                    stats.echo_return_loss ? std::to_string(*stats.echo_return_loss).c_str() : "-",
                    stats.echo_return_loss_enhancement ? std::to_string(*stats.echo_return_loss_enhancement).c_str() : "-",
                    stats.divergent_filter_fraction ? std::to_string(*stats.divergent_filter_fraction).c_str() : "-",
                    stats.residual_echo_likelihood ? std::to_string(*stats.residual_echo_likelihood).c_str() : "-",
                    stats.delay_ms ? std::to_string(*stats.delay_ms).c_str() : "-");
        std::fflush(stdout);
    }

private:
    void onPcm(const int16_t *data, size_t samplesPerChannel, int rate,
               size_t channels)
    {
        if (rate != kSampleRate || channels < 1) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true))
                std::fprintf(stderr, "不支持的采集格式: %dHz/%zu声道\n",
                             rate, channels);
            return;
        }

        std::vector<int16_t> mono(samplesPerChannel);
        for (size_t i = 0; i < samplesPerChannel; ++i) {
            int sum = 0;
            for (size_t channel = 0; channel < channels; ++channel)
                sum += data[i * channels + channel];
            mono[i] = static_cast<int16_t>(sum / static_cast<int>(channels));
        }

        // 20ms 一块 → 两个 10ms 的 APM 帧。
        std::vector<int16_t> reference;
        for (size_t offset = 0; offset + kFrameSamples <= mono.size();
             offset += kFrameSamples) {
            const int16_t *near = mono.data() + offset;

            m_transport.takeReference(reference);
            double referenceSum = 0.0;
            for (int16_t sample : reference)
                referenceSum += static_cast<double>(sample) * sample;
            m_referenceSquares += referenceSum;
            m_referenceSamples += kFrameSamples;

            std::vector<int16_t> reverseOut(kFrameSamples);
            m_apm->ProcessReverseStream(reference.data(),
                                        webrtc::StreamConfig(kSampleRate, 1),
                                        webrtc::StreamConfig(kSampleRate, 1),
                                        reverseOut.data());

            std::vector<int16_t> aecOut(kFrameSamples);
            m_apm->ProcessStream(near, webrtc::StreamConfig(kSampleRate, 1),
                                 webrtc::StreamConfig(kSampleRate, 1),
                                 aecOut.data());

            QMutexLocker locker(&m_measureMutex);
            m_measurement.rawVoice.process(near, kFrameSamples);
            m_measurement.rawOther.process(near, kFrameSamples);
            m_measurement.aecVoice.process(aecOut.data(), kFrameSamples);
            m_measurement.aecOther.process(aecOut.data(), kFrameSamples);
        }
    }

    bool m_ok = true;

    ToneTransport m_transport;
    std::optional<webrtc::Environment> m_environment;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> m_adm;
    webrtc::scoped_refptr<webrtc::AudioProcessing> m_apm;

    std::unique_ptr<PulseMonitorCapture> m_capture;

    QMutex m_measureMutex;
    Measurement m_measurement;

    std::atomic<double> m_referenceSquares{0.0};
    std::atomic<uint64_t> m_referenceSamples{0};
};

// 子进程：用 libpulse 的 pa_simple 播放 4kHz，扮演「其他应用」。
//
// 必须独立进程：Linux 的虚拟 Sink 路由按进程 PID 排除 Syncine 自身，
// 同进程的播放流会被当成「自己人」而留在真实 Sink。
// 不用 QAudioSink 的原因：Qt Multimedia 的 Pulse 流带 DONT_MOVE，
// 服务端拒绝把它移到虚拟 Sink（实测 EINVAL）；pa_simple 没有这个限制，
// 更接近浏览器/播放器等普通应用。
int runOtherToneProcess(QCoreApplication &app)
{
    Q_UNUSED(app);

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = kSampleRate;
    spec.channels = kChannels;

    int error = 0;
    pa_simple *stream = pa_simple_new(
        nullptr, "SyncineOtherApp", PA_STREAM_PLAYBACK, nullptr, "other-tone",
        &spec, nullptr, nullptr, &error);
    if (stream == nullptr) {
        std::fprintf(stderr, "other-tone: pa_simple_new 失败: %s\n",
                     pa_strerror(error));
        return 1;
    }

    constexpr int kBlockFrames = kSampleRate / 50; // 20ms
    std::vector<int16_t> block(static_cast<size_t>(kBlockFrames) * kChannels);
    const double omega = 2.0 * M_PI * kOtherAppHz / kSampleRate;
    int64_t frameIndex = 0;

    // 12 秒；父进程 6 秒后 terminate。写满前一直阻塞在这里。
    for (int blockIndex = 0; blockIndex < 600; ++blockIndex) {
        for (int frame = 0; frame < kBlockFrames; ++frame) {
            const int16_t value = static_cast<int16_t>(
                std::sin(omega * frameIndex) * 800.0);
            block[static_cast<size_t>(frame) * 2] = value;
            block[static_cast<size_t>(frame) * 2 + 1] = value;
            ++frameIndex;
        }
        if (pa_simple_write(stream, block.data(),
                            block.size() * sizeof(int16_t), &error) < 0) {
            std::fprintf(stderr, "other-tone: 写入失败: %s\n",
                         pa_strerror(error));
            break;
        }
    }

    pa_simple_free(stream);
    return 0;
}

// 离线自检：同一套 APM 用法，参考信号与近端信号完全在内存里生成。
// 用来区分「APM 用法/配置问题」与「真实回路的参考对齐问题」。
int runApmSelfTest(int delayMs)
{
    auto environment = webrtc::CreateEnvironment();

    webrtc::AudioProcessing::Config config;
    config.echo_canceller.enabled = true;
    config.echo_canceller.enforce_high_pass_filtering = false;
    config.noise_suppression.enabled = false;
    config.high_pass_filter.enabled = false;
    config.gain_controller1.enabled = false;
    config.gain_controller2.enabled = false;

    webrtc::BuiltinAudioProcessingBuilder builder(config);
    auto apm = builder.Build(environment);
    if (apm == nullptr)
        return 1;
    apm->Initialize();
    apm->set_stream_delay_ms(delayMs);

    constexpr int seconds = 6;
    const size_t total = static_cast<size_t>(kSampleRate) * seconds;
    const size_t delaySamples =
        static_cast<size_t>(kSampleRate) * static_cast<size_t>(delayMs) / 1000;

    std::vector<int16_t> reference(total, 0);
    uint32_t rng = 0x2468ace0;
    for (size_t i = 0; i < total; ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const int32_t noise = static_cast<int32_t>(rng >> 16) - 32768;
        reference[i] = static_cast<int16_t>(noise * 0.35);
    }

    std::vector<int16_t> near(total, 0);
    for (size_t i = 0; i < total; ++i) {
        const int32_t echo = i >= delaySamples
                                 ? reference[i - delaySamples]
                                 : 0;
        const int32_t other = static_cast<int32_t>(
            std::sin(2.0 * M_PI * kOtherAppHz * i / kSampleRate) * 800.0);
        near[i] = static_cast<int16_t>(
            std::clamp(echo + other, -32768, 32767));
    }

    BandMeter rawVoice;
    BandMeter aecVoice;
    ToneMeter rawOther;
    ToneMeter aecOther;
    rawOther.freq = kOtherAppHz;
    aecOther.freq = kOtherAppHz;

    std::vector<int16_t> out(kFrameSamples);
    for (size_t offset = 0; offset + kFrameSamples <= total;
         offset += kFrameSamples) {
        if (offset < static_cast<size_t>(kSampleRate) * 3) {
            // 前 3 秒只做自适应，不计入测量。
            if (offset + kFrameSamples >= static_cast<size_t>(kSampleRate) * 3) {
                rawVoice.reset();
                aecVoice.reset();
                rawOther.reset();
                aecOther.reset();
            }
        }

        apm->ProcessReverseStream(reference.data() + offset,
                                  webrtc::StreamConfig(kSampleRate, 1),
                                  webrtc::StreamConfig(kSampleRate, 1),
                                  out.data());
        const int result =
            apm->ProcessStream(near.data() + offset,
                               webrtc::StreamConfig(kSampleRate, 1),
                               webrtc::StreamConfig(kSampleRate, 1),
                               out.data());
        if (result != 0) {
            std::fprintf(stderr, "selftest ProcessStream 返回 %d\n", result);
            return 1;
        }

        if (offset >= static_cast<size_t>(kSampleRate) * 3) {
            rawVoice.process(near.data() + offset, kFrameSamples);
            aecVoice.process(out.data(), kFrameSamples);
            rawOther.process(near.data() + offset, kFrameSamples);
            aecOther.process(out.data(), kFrameSamples);
        }
    }

    const double attenuation = toDb(rawVoice.power()) - toDb(aecVoice.power());
    const double otherChange =
        std::fabs(toDb(aecOther.power()) - toDb(rawOther.power()));
    std::printf("selftest delay=%dms attenuation=%.1fdB other_change=%.1fdB "
                "raw_voice=%.1fdB aec_voice=%.1fdB\n",
                delayMs, attenuation, otherChange,
                toDb(rawVoice.power()), toDb(aecVoice.power()));
    return attenuation > 15.0 ? 0 : 2;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    if (app.arguments().contains(QStringLiteral("--selftest"))) {
        return runApmSelfTest(50);
    }
    if (app.arguments().contains(QStringLiteral("--other-tone"))) {
        return runOtherToneProcess(app);
    }

    Probe probe;
    if (!probe.ok())
        return 1;

    // 独立进程扮演「其他应用」：虚拟 Sink 路由按 PID 排除本进程，
    // 同进程的播放流会被当成自己人，测不出采集效果。
    // 在采集启动之后再起子进程，专门覆盖「共享期间新出现的流」这条订阅路径。
    QProcess otherTone;
    otherTone.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--other-tone")});
    if (!otherTone.waitForStarted(2000)) {
        std::fprintf(stderr, "other-tone 子进程启动失败: %s\n",
                     qPrintable(otherTone.errorString()));
    }

    Snapshot voiceOn;
    Snapshot voiceOff;

    // 0–2s 预热；2–4s 测量「房间语音在播」；4–6s 测量「本机静音房间语音」。
    QTimer::singleShot(2000, &app, [&] {
        probe.resetMeasurement();
        std::printf("warm-up 完成，开始测量 voice-on\n");
        std::fflush(stdout);
    });

    QTimer::singleShot(4000, &app, [&] {
        voiceOn = probe.snapshot();
        probe.printApmStats("voice-on");
        probe.setVoiceMuted(true);
        probe.resetMeasurement();
        std::printf("phase=voice-on  rawVoice=%.1fdB rawOther=%.1fdB "
                    "aecVoice=%.1fdB aecOther=%.1fdB maxRefQueue=%zu "
                    "refRms=%.1fdB\n",
                    voiceOn.rawVoiceDb, voiceOn.rawOtherDb,
                    voiceOn.aecVoiceDb, voiceOn.aecOtherDb,
                    probe.maxReferenceQueue(), probe.referenceRmsDb());
        std::fflush(stdout);
    });

    QTimer::singleShot(6000, &app, [&] {
        voiceOff = probe.snapshot();
        probe.printApmStats("voice-off");
        std::printf("phase=voice-off rawVoice=%.1fdB rawOther=%.1fdB "
                    "aecVoice=%.1fdB aecOther=%.1fdB\n",
                    voiceOff.rawVoiceDb, voiceOff.rawOtherDb,
                    voiceOff.aecVoiceDb, voiceOff.aecOtherDb);
        std::fflush(stdout);
        app.quit();
    });

    app.exec();

    otherTone.terminate();
    if (!otherTone.waitForFinished(1000))
        otherTone.kill();

    // ---- 判定 ----
    const bool isolated = probe.isolated();
    const double voiceChange =
        voiceOn.rawVoiceDb - voiceOff.rawVoiceDb; // 房间语音被采回的量
    const double otherLevel = voiceOn.rawOtherDb;

    std::printf("isolation=%d voice_change=%.1fdB other_level=%.1fdB\n",
                isolated ? 1 : 0, voiceChange, otherLevel);

    if (isolated) {
        // 隔离模式：房间语音（ADM）不应随开关变化；其他应用必须能采到。
        if (std::fabs(voiceChange) > 6.0) {
            std::fprintf(stderr,
                         "FAIL: 隔离后房间语音仍被采入（开关前后差 %.1fdB）\n",
                         voiceChange);
            return 1;
        }
        if (otherLevel < -40.0) {
            std::fprintf(stderr,
                         "FAIL: 其他应用的声音没有被采集（%.1fdB）\n",
                         otherLevel);
            return 1;
        }
        std::printf("echo_probe: 虚拟 Sink 隔离有效，其他应用声音正常采集\n");
        return 0;
    }

    // 回退模式（SYNCINE_SCREEN_AUDIO_ISOLATE=0）：应能复现房间语音被采回。
    if (voiceChange < 10.0) {
        std::fprintf(stderr,
                     "FAIL: 回退模式没有复现回声（%.1fdB），"
                     "设备/音量可能不适用\n", voiceChange);
        return 1;
    }
    std::printf("echo_probe: 回退模式复现了房间语音回声（对照成立）\n");
    return 0;
}
