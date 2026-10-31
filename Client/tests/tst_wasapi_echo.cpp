//
// Windows 系统声音采集的「排除自身」验证（手动运行，需要真实音频设备）。
//
// 背景：屏幕共享采集系统声音时，如果把本软件自己播放的房间语音也采进去，
// 观众会听到自己的声音被延迟回放，形成永久回声。Windows 后端用 WASAPI
// 进程回环 + EXCLUDE_TARGET_PROCESS_TREE 排除本进程树。
//
// 这个探针用 PlaySound 在本进程内播放 1kHz 单音，分别验证：
//   1. 进程回环模式：单音来自本进程，应当被排除，采集到的基本是静音；
//   2. 整机 loopback 模式（环境变量强制回退）：同一条单音应被采集到。
// 第 2 步是控制组：证明 PlaySound 确实在出声、采集链路确实工作，
// 否则「静音」可能只是没采到任何数据。
//
// 用法：
//   syncine_wasapi_echo.exe
// 判定：进程回环 RMS 应远低于整机 loopback 的 RMS；两个模式都要有数据回调。
//

#include <QCoreApplication>
#include <QGuiApplication>
#include <QString>
#include <QtGlobal>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <mmsystem.h>
#endif

#include "capture/WasapiLoopbackCapture.h"

#if !defined(Q_OS_WIN)
int main()
{
    std::printf("this probe is Windows-only\n");
    return 0;
}
#else

namespace {

constexpr int kToneMs = 1200;
constexpr int kToneRate = 48000;
constexpr int kToneChannels = 2;

struct Accumulator {
    std::atomic<long long> samples{0};
    std::atomic<long long> sumSquares{0};
    std::atomic<int> callbacks{0};

    void reset()
    {
        samples.store(0, std::memory_order_relaxed);
        sumSquares.store(0, std::memory_order_relaxed);
        callbacks.store(0, std::memory_order_relaxed);
    }

    double rms() const
    {
        const long long n = samples.load(std::memory_order_relaxed);
        if (n <= 0)
            return 0.0;
        return std::sqrt(static_cast<double>(
                             sumSquares.load(std::memory_order_relaxed))
                         / static_cast<double>(n));
    }
};

void append16(std::vector<unsigned char> &out, unsigned value)
{
    out.push_back(static_cast<unsigned char>(value));
    out.push_back(static_cast<unsigned char>(value >> 8));
}

void append32(std::vector<unsigned char> &out, unsigned value)
{
    append16(out, value & 0xFFFF);
    append16(out, value >> 16);
}

std::vector<unsigned char> makeToneWav()
{
    const int frames = kToneRate * kToneMs / 1000;
    const unsigned dataBytes = static_cast<unsigned>(frames * kToneChannels * 2);

    std::vector<unsigned char> wav;
    wav.reserve(dataBytes + 44);
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    append32(wav, 36 + dataBytes);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    append32(wav, 16);
    append16(wav, WAVE_FORMAT_PCM);
    append16(wav, kToneChannels);
    append32(wav, kToneRate);
    append32(wav, kToneRate * kToneChannels * 2);
    append16(wav, kToneChannels * 2);
    append16(wav, 16);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    append32(wav, dataBytes);

    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kToneRate;
        const int16_t v = static_cast<int16_t>(
            std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t) * 8000.0);
        append16(wav, static_cast<unsigned>(static_cast<uint16_t>(v)));
        append16(wav, static_cast<unsigned>(static_cast<uint16_t>(v)));
    }
    return wav;
}

bool waitForIsolation(const WasapiLoopbackCapture &capture, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 20) {
        if (capture.isolatesOwnAudio())
            return true;
        Sleep(20);
    }
    return capture.isolatesOwnAudio();
}

bool runMode(const char *name, bool forceFullLoopback, const std::vector<unsigned char> &wav,
             double *rmsOut)
{
    if (forceFullLoopback)
        qputenv("SYNCINE_SCREEN_AUDIO_PROCESS_LOOPBACK", "0");
    else
        qunsetenv("SYNCINE_SCREEN_AUDIO_PROCESS_LOOPBACK");

    Accumulator acc;
    WasapiLoopbackCapture capture;
    QString error;
    const bool started = capture.start(
        QString(),
        [&acc](const int16_t *data, size_t samplesPerChannel, int, size_t channels) {
            const long long count =
                static_cast<long long>(samplesPerChannel * channels);
            long long sum = 0;
            for (long long i = 0; i < count; ++i) {
                const long long v = data[i];
                sum += v * v;
            }
            acc.samples.fetch_add(count, std::memory_order_relaxed);
            acc.sumSquares.fetch_add(sum, std::memory_order_relaxed);
            acc.callbacks.fetch_add(1, std::memory_order_relaxed);
        },
        [&error](const QString &message) { error = message; });

    if (!started) {
        std::printf("[%s] start failed: %s\n", name, qPrintable(error));
        return false;
    }

    if (!forceFullLoopback && !waitForIsolation(capture, 3000)) {
        std::printf("[%s] process loopback not active (fell back to full loopback)\n",
                    name);
        capture.stop();
        return false;
    }

    Sleep(250);
    PlaySoundA(reinterpret_cast<LPCSTR>(wav.data()), nullptr,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    Sleep(kToneMs + 300);
    PlaySoundA(nullptr, nullptr, 0);

    const bool isolated = capture.isolatesOwnAudio();
    capture.stop();

    *rmsOut = acc.rms();
    std::printf("[%s] callbacks=%d samples=%lld rms=%.1f isolated=%d error=%s\n",
                name, acc.callbacks.load(), acc.samples.load(), *rmsOut,
                isolated ? 1 : 0, error.isEmpty() ? "-" : qPrintable(error));
    return acc.callbacks.load() > 0 && acc.samples.load() > 0;
}

} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    const std::vector<unsigned char> wav = makeToneWav();

    double isolatedRms = 0.0;
    double fullRms = 0.0;
    int failures = 0;

    if (!runMode("process-loopback", /*forceFullLoopback=*/false, wav, &isolatedRms)) {
        std::printf("FAIL: process loopback did not run\n");
        return 1;
    }
    if (!runMode("full-loopback", /*forceFullLoopback=*/true, wav, &fullRms)) {
        std::printf("FAIL: full loopback control did not capture any data\n");
        return 1;
    }

    // 控制组必须明显采到单音；实验组应只剩底噪。
    if (fullRms < 1000.0) {
        std::printf("FAIL: control full-loopback rms %.1f too low (tone not captured?)\n",
                    fullRms);
        ++failures;
    }
    if (isolatedRms > fullRms * 0.05 || isolatedRms > 300.0) {
        std::printf("FAIL: own audio not excluded: isolated rms %.1f vs full %.1f\n",
                    isolatedRms, fullRms);
        ++failures;
    }

    if (failures != 0) {
        std::printf("wasapi echo probe: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("wasapi echo probe: passed (own rms %.1f, full rms %.1f)\n",
                isolatedRms, fullRms);
    return 0;
}

#endif
