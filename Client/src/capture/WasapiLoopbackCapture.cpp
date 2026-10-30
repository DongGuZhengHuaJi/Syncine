//
// Created by donggu on 2026/10/30.
//

// 进程回环 API（AUDIOCLIENT_ACTIVATION_PARAMS / ActivateAudioInterfaceAsync）
// 需要 Windows 10 的头文件与 _WIN32_WINNT。必须在任何 Windows 头文件之前
// 定义，Qt 的 Windows 头可能会先引入 windows.h。
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00
#endif
#if defined(_WIN32) && !defined(WINVER)
#define WINVER 0x0A00
#endif
#if defined(_WIN32) && !defined(NTDDI_VERSION)
#define NTDDI_VERSION 0x0A000000  // NTDDI_WIN10
#endif

#include "WasapiLoopbackCapture.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// ksmedia.h 在新版 Windows SDK 里要求先包含 ks.h（否则 C1189）。
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#include <wrl/implements.h>
#define SYNCINE_HAVE_PROCESS_LOOPBACK 1
#else
#define SYNCINE_HAVE_PROCESS_LOOPBACK 0
#endif

#include "core/Log.h"

namespace {

using Microsoft::WRL::ComPtr;

bool isFloatFormat(const WAVEFORMATEX *format, WORD *validBits)
{
    WORD bits = format->wBitsPerSample;
    bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto *extended =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        isFloat = IsEqualGUID(extended->SubFormat,
                              KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        if (extended->Samples.wValidBitsPerSample != 0)
            bits = extended->Samples.wValidBitsPerSample;
    }

    if (validBits != nullptr)
        *validBits = bits;
    return isFloat;
}

float readSample(const BYTE *data, UINT32 frame, UINT32 channel,
                 UINT32 channels, bool isFloat, WORD bits)
{
    if (isFloat) {
        return reinterpret_cast<const float *>(data)[
            static_cast<size_t>(frame) * channels + channel];
    }
    if (bits <= 16) {
        return reinterpret_cast<const int16_t *>(data)[
                   static_cast<size_t>(frame) * channels + channel]
               / 32768.0f;
    }
    return static_cast<float>(
               reinterpret_cast<const int32_t *>(data)[
                   static_cast<size_t>(frame) * channels + channel]
           ) / 2147483648.0f;
}

#if SYNCINE_HAVE_PROCESS_LOOPBACK

struct ProcessLoopbackState {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    HRESULT result = E_FAIL;
    Microsoft::WRL::ComPtr<IAudioClient> client;

    // 激活参数必须活到异步激活完成。放在共享状态里，即使 5 秒超时、
    // 调用线程已经返回，回调线程仍然能安全读到它。
    AUDIOCLIENT_ACTIVATION_PARAMS activationParams = {};
    PROPVARIANT propVariant = {};
};

// ActivateAudioInterfaceAsync 是异步的：用 RuntimeClass 实现 COM 回调，
// 激活完成后把 IAudioClient 交给等待线程。
class ProcessLoopbackHandler
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IActivateAudioInterfaceCompletionHandler> {
public:
    explicit ProcessLoopbackHandler(std::shared_ptr<ProcessLoopbackState> state)
        : m_state(std::move(state))
    {
    }

    STDMETHOD(ActivateCompleted)
    (IActivateAudioInterfaceAsyncOperation *operation) override
    {
        if (operation == nullptr)
            return E_POINTER;

        Microsoft::WRL::ComPtr<IUnknown> activated;
        HRESULT activateResult = E_FAIL;
        const HRESULT queryResult =
            operation->GetActivateResult(&activateResult,
                                         activated.GetAddressOf());

        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->done = true;
            m_state->result =
                SUCCEEDED(queryResult) ? activateResult : queryResult;
            if (SUCCEEDED(m_state->result) && activated.Get() != nullptr)
                activated.As(&m_state->client);
        }
        m_state->condition.notify_all();
        return S_OK;
    }

private:
    std::shared_ptr<ProcessLoopbackState> m_state;
};

// 激活「进程回环」虚拟设备，并排除本进程树（含 yt-dlp/ffmpeg 等子进程）。
// 成功时返回一个尚未 Initialize 的 IAudioClient。
bool activateProcessLoopback(Microsoft::WRL::ComPtr<IAudioClient> *client,
                             QString *error)
{
    auto state = std::make_shared<ProcessLoopbackState>();
    auto handler = Microsoft::WRL::Make<ProcessLoopbackHandler>(state);
    if (handler == nullptr) {
        *error = QStringLiteral("创建进程回环回调失败");
        return false;
    }

    state->activationParams.ActivationType =
        AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    state->activationParams.ProcessLoopbackParams.TargetProcessId =
        GetCurrentProcessId();
    state->activationParams.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    state->propVariant.vt = VT_BLOB;
    state->propVariant.blob.cbSize =
        static_cast<ULONG>(sizeof(state->activationParams));
    state->propVariant.blob.pBlobData =
        reinterpret_cast<BYTE *>(&state->activationParams);

    Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    const HRESULT hr = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
        &state->propVariant, handler.Get(), operation.GetAddressOf());
    if (FAILED(hr)) {
        *error = QStringLiteral("ActivateAudioInterfaceAsync 失败：0x%1")
                     .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        return false;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_for(lock, std::chrono::seconds(5),
                                   [&state] { return state->done; })) {
        *error = QStringLiteral("等待进程回环激活超时");
        return false;
    }
    if (FAILED(state->result) || state->client == nullptr) {
        *error = QStringLiteral("进程回环激活失败：0x%1")
                     .arg(static_cast<quint32>(state->result), 8, 16,
                          QLatin1Char('0'));
        return false;
    }

    *client = state->client;
    return true;
}

#endif // SYNCINE_HAVE_PROCESS_LOOPBACK

} // namespace

WasapiLoopbackCapture::~WasapiLoopbackCapture()
{
    stop();
}

bool WasapiLoopbackCapture::isSupported()
{
    return true;
}

bool WasapiLoopbackCapture::start(const QString &outputDeviceName,
                                  PcmCallback pcmCallback,
                                  ErrorCallback errorCallback)
{
    stop();

    if (!pcmCallback) {
        if (errorCallback)
            errorCallback(QStringLiteral("系统声音回调未设置"));
        return false;
    }

    // 在 GUI 线程解析 Qt 设备 id（Windows 上就是 WASAPI endpoint id）。
    // 解析不到就退回默认输出设备。
    QString endpointId;
    QAudioDevice output;
    if (outputDeviceName.isEmpty()) {
        output = QMediaDevices::defaultAudioOutput();
    } else {
        for (const QAudioDevice &device : QMediaDevices::audioOutputs()) {
            if (device.description() == outputDeviceName) {
                output = device;
                break;
            }
        }
    }
    if (!output.isNull())
        endpointId = QString::fromUtf8(output.id());

    m_pcmCallback = std::move(pcmCallback);
    m_errorCallback = std::move(errorCallback);
    m_stop.store(false, std::memory_order_relaxed);

    try {
        m_thread = std::thread(&WasapiLoopbackCapture::threadLoop, this,
                               endpointId);
    } catch (const std::exception &error) {
        if (m_errorCallback)
            m_errorCallback(QStringLiteral("启动系统声音线程失败：%1")
                                .arg(QString::fromUtf8(error.what())));
        return false;
    }

    LOG_INFO("ScreenAudio") << "开始采集系统声音（WASAPI，模式由采集线程确定）";
    return true;
}

void WasapiLoopbackCapture::stop()
{
    m_stop.store(true, std::memory_order_relaxed);
    if (m_thread.joinable())
        m_thread.join();
}

void WasapiLoopbackCapture::threadLoop(QString endpointId)
{
    HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(comResult);

    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX *mixFormat = nullptr;
    bool started = false;
    bool processLoopback = false;

    auto fail = [this](const QString &message) {
        if (!m_stop.load(std::memory_order_relaxed) && m_errorCallback)
            m_errorCallback(message);
    };

    auto resetClient = [&] {
        capture.Reset();
        client.Reset();
        if (mixFormat != nullptr) {
            CoTaskMemFree(mixFormat);
            mixFormat = nullptr;
        }
        started = false;
    };

    // 整机 loopback：打开指定（或默认）输出端点的 IAudioClient。
    auto openEndpoint = [&](QString *error) -> bool {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                      CLSCTX_ALL,
                                      IID_PPV_ARGS(enumerator.GetAddressOf()));
        if (FAILED(hr)) {
            *error = QStringLiteral("创建 WASAPI 设备枚举器失败：0x%1")
                         .arg(static_cast<quint32>(hr), 8, 16,
                              QLatin1Char('0'));
            return false;
        }

        if (!endpointId.isEmpty()) {
            const std::wstring id = endpointId.toStdWString();
            enumerator->GetDevice(id.c_str(), &device);
        }
        if (device == nullptr)
            enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (device == nullptr) {
            *error = QStringLiteral("找不到系统输出设备");
            return false;
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(
                                  client.GetAddressOf()));
        if (FAILED(hr)) {
            *error = QStringLiteral("打开 WASAPI 输出设备失败：0x%1")
                         .arg(static_cast<quint32>(hr), 8, 16,
                              QLatin1Char('0'));
            return false;
        }
        return true;
    };

    auto prepare = [&](bool useProcessLoopback, QString *error) -> bool {
        resetClient();

        if (useProcessLoopback) {
#if SYNCINE_HAVE_PROCESS_LOOPBACK
            if (!activateProcessLoopback(&client, error))
                return false;
#else
            *error = QStringLiteral("当前 Windows SDK 不支持进程回环");
            return false;
#endif
        } else if (!openEndpoint(error)) {
            return false;
        }

        HRESULT hr = client->GetMixFormat(&mixFormat);
        if (FAILED(hr) || mixFormat == nullptr) {
            *error = QStringLiteral("读取 WASAPI 混音格式失败");
            return false;
        }

        // 进程回环要求 buffer duration 为 0；普通 loopback 沿用 100ms。
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK,
                                useProcessLoopback ? 0 : 1000000, 0,
                                mixFormat, nullptr);
        if (FAILED(hr)) {
            *error = QStringLiteral("初始化 WASAPI loopback 失败：0x%1")
                         .arg(static_cast<quint32>(hr), 8, 16,
                              QLatin1Char('0'));
            return false;
        }

        hr = client->GetService(IID_PPV_ARGS(capture.GetAddressOf()));
        if (FAILED(hr) || capture == nullptr) {
            *error = QStringLiteral("获取 WASAPI 采集接口失败");
            return false;
        }

        hr = client->Start();
        if (FAILED(hr)) {
            *error = QStringLiteral("启动 WASAPI loopback 失败");
            return false;
        }

        started = true;
        return true;
    };

    do {
        // 默认优先进程回环；环境变量 SYNCINE_SCREEN_AUDIO_PROCESS_LOOPBACK=0
        // 可强制回退到整机 loopback（排查用）。
        const bool wantProcessLoopback =
            !qEnvironmentVariableIsSet("SYNCINE_SCREEN_AUDIO_PROCESS_LOOPBACK")
            || qEnvironmentVariableIntValue(
                   "SYNCINE_SCREEN_AUDIO_PROCESS_LOOPBACK") != 0;

        bool prepared = false;
        if (wantProcessLoopback) {
            QString processError;
            prepared = prepare(true, &processError);
            if (prepared) {
                processLoopback = true;
                LOG_INFO("ScreenAudio")
                    << "系统声音采集：进程回环（排除 Syncine 自身进程树）";
            } else {
                LOG_WARN("ScreenAudio")
                    << "进程回环不可用，回退到整机 loopback：" << processError;
            }
        }

        if (!prepared) {
            QString endpointError;
            if (!prepare(false, &endpointError)) {
                fail(QStringLiteral("打开系统声音采集失败：%1")
                         .arg(endpointError));
                break;
            }
            processLoopback = false;
            LOG_WARN("ScreenAudio")
                << "系统声音采集：整机 loopback（可能包含本软件播放的声音）";
        }

        m_processLoopback.store(processLoopback, std::memory_order_relaxed);

        HRESULT hr = S_OK;
        WORD bits = 0;
        const bool isFloat = isFloatFormat(mixFormat, &bits);
        const UINT32 srcChannels = mixFormat->nChannels;
        const UINT32 srcRate = mixFormat->nSamplesPerSec;

        std::vector<int16_t> output;
        while (!m_stop.load(std::memory_order_relaxed)) {
            UINT32 packetFrames = 0;
            hr = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                fail(QStringLiteral("读取 WASAPI 包大小失败"));
                break;
            }
            if (packetFrames == 0) {
                Sleep(5);
                continue;
            }

            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                fail(QStringLiteral("读取 WASAPI 数据包失败"));
                break;
            }

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const UINT32 outFrames = srcRate == 48000
                                         ? frames
                                         : static_cast<UINT32>(
                                               std::llround(frames
                                                            * 48000.0
                                                            / srcRate));
            output.resize(static_cast<size_t>(outFrames) * 2);

            for (UINT32 frame = 0; frame < outFrames; ++frame) {
                float left = 0.0f;
                float right = 0.0f;

                if (!silent && data != nullptr) {
                    const double srcPos = srcRate == 48000
                                              ? frame
                                              : frame * (double)srcRate / 48000.0;
                    const UINT32 i0 = std::min<UINT32>(
                        static_cast<UINT32>(srcPos), frames > 0 ? frames - 1 : 0);
                    const UINT32 i1 = std::min<UINT32>(i0 + 1,
                                                       frames > 0 ? frames - 1 : 0);
                    const float frac = static_cast<float>(srcPos - i0);

                    auto channelSample = [&](UINT32 sourceFrame, UINT32 channel) {
                        if (channel >= srcChannels)
                            channel = srcChannels - 1;
                        return readSample(data, sourceFrame, channel,
                                          srcChannels, isFloat, bits);
                    };

                    const float left0 = channelSample(i0, 0);
                    const float right0 = srcChannels > 1
                                             ? channelSample(i0, 1)
                                             : left0;
                    const float left1 = channelSample(i1, 0);
                    const float right1 = srcChannels > 1
                                             ? channelSample(i1, 1)
                                             : left1;
                    left = left0 + (left1 - left0) * frac;
                    right = right0 + (right1 - right0) * frac;
                }

                output[static_cast<size_t>(frame) * 2] = static_cast<int16_t>(
                    std::clamp(left, -1.0f, 1.0f) * 32767.0f);
                output[static_cast<size_t>(frame) * 2 + 1] = static_cast<int16_t>(
                    std::clamp(right, -1.0f, 1.0f) * 32767.0f);
            }

            capture->ReleaseBuffer(frames);

            if (m_pcmCallback != nullptr && !output.empty()) {
                m_pcmCallback(output.data(), outFrames, 48000, 2);
            }
        }
    } while (false);

    m_processLoopback.store(false, std::memory_order_relaxed);
    if (started && client != nullptr)
        client->Stop();
    resetClient();

    if (comInitialized)
        CoUninitialize();

    LOG_INFO("ScreenAudio") << "系统声音采集线程结束";
}
