//
// Created by donggu on 2026/10/30.
//

#include "PulseMonitorCapture.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <QtGlobal>

#include <pulse/error.h>
#include <pulse/simple.h>

#include <vector>

#include "PulseSinkRouter.h"
#include "core/Log.h"

PulseMonitorCapture::PulseMonitorCapture() = default;

PulseMonitorCapture::~PulseMonitorCapture()
{
    stop();
}

bool PulseMonitorCapture::isSupported()
{
    return true;
}

bool PulseMonitorCapture::start(const QString &outputDeviceName,
                                PcmCallback pcmCallback,
                                ErrorCallback errorCallback)
{
    stop();

    if (!pcmCallback) {
        if (errorCallback)
            errorCallback(QStringLiteral("系统声音回调未设置"));
        return false;
    }

    // Qt 的 Linux 输出设备 id 就是 PulseAudio 的 sink 名；
    // monitor source 的名字固定为 "<sink>.monitor"。
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

    if (output.isNull()) {
        if (errorCallback)
            errorCallback(QStringLiteral("未找到输出设备，无法采集系统声音"));
        return false;
    }

    const QString sinkId = QString::fromUtf8(output.id());
    if (sinkId.isEmpty()) {
        if (errorCallback)
            errorCallback(QStringLiteral("输出设备没有 PulseAudio sink 名"));
        return false;
    }

    // 默认建立虚拟 Sink 并接管其他应用的流，使采集里不含本软件播放的
    // 房间语音。模块加载失败（权限、旧版服务端、非 PipeWire/Pulse 环境）
    // 时回退到整机 monitor —— 功能优先，界面会通过日志/提示说明可能回声。
    QString monitorSource;
    m_isolated = false;
    const bool isolateRequested =
        !qEnvironmentVariableIsSet("SYNCINE_SCREEN_AUDIO_ISOLATE")
        || qEnvironmentVariableIntValue("SYNCINE_SCREEN_AUDIO_ISOLATE") != 0;
    if (isolateRequested) {
        m_router = std::make_unique<PulseSinkRouter>();
        QString routerError;
        QString virtualMonitor;
        if (m_router->start(sinkId, &virtualMonitor, &routerError)) {
            monitorSource = virtualMonitor;
            m_isolated = true;
            LOG_INFO("ScreenAudio") << "系统声音隔离已启用，采集虚拟 Sink:"
                                    << virtualMonitor;
        } else {
            m_router.reset();
            LOG_WARN("ScreenAudio")
                << "虚拟 Sink 路由不可用，回退到整机 monitor:" << routerError;
        }
    } else {
        LOG_INFO("ScreenAudio") << "SYNCINE_SCREEN_AUDIO_ISOLATE=0，使用整机 monitor";
    }

    if (monitorSource.isEmpty())
        monitorSource = sinkId + QStringLiteral(".monitor");

    m_pcmCallback = std::move(pcmCallback);
    m_errorCallback = std::move(errorCallback);
    m_stop.store(false, std::memory_order_relaxed);

    try {
        m_thread = std::thread(&PulseMonitorCapture::threadLoop, this,
                               monitorSource);
    } catch (const std::exception &error) {
        m_router.reset();
        m_isolated = false;
        if (m_errorCallback)
            m_errorCallback(QStringLiteral("启动系统声音线程失败：%1")
                                .arg(QString::fromUtf8(error.what())));
        return false;
    }

    LOG_INFO("ScreenAudio") << "开始采集系统声音:" << monitorSource;
    return true;
}

void PulseMonitorCapture::stop()
{
    m_stop.store(true, std::memory_order_relaxed);
    if (m_thread.joinable())
        m_thread.join();

    // 采集线程先停，再拆虚拟 Sink；否则 monitor source 会先消失。
    if (m_router) {
        m_router->stop();
        m_router.reset();
    }
    m_isolated = false;
}

void PulseMonitorCapture::threadLoop(QString monitorSource)
{
    pa_sample_spec spec;
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = 48000;
    spec.channels = 2;

    // 必须显式给 fragsize。pa_simple 的默认缓冲会攒到接近 2 秒才交付第一块，
    // 共享系统声音会出现明显延迟；20ms 一块与 MovieAudioSource 的 10ms
    // 切块也更容易衔接。
    constexpr int kFrameMs = 20;
    constexpr int kFramesPerBlock = 48000 * kFrameMs / 1000;
    constexpr int kChannels = 2;
    pa_buffer_attr bufferAttr;
    bufferAttr.maxlength = static_cast<uint32_t>(-1);
    bufferAttr.tlength = static_cast<uint32_t>(-1);
    bufferAttr.prebuf = static_cast<uint32_t>(-1);
    bufferAttr.minreq = static_cast<uint32_t>(-1);
    bufferAttr.fragsize = kFramesPerBlock * kChannels * sizeof(int16_t);

    int error = 0;
    const QByteArray sourceName = monitorSource.toUtf8();
    pa_simple *handle = pa_simple_new(
        nullptr, "Syncine", PA_STREAM_RECORD, sourceName.constData(),
        "screen audio", &spec, nullptr, &bufferAttr, &error);

    if (handle == nullptr) {
        if (!m_stop.load(std::memory_order_relaxed) && m_errorCallback) {
            m_errorCallback(QStringLiteral("无法打开系统声音监视源 %1：%2")
                                .arg(monitorSource)
                                .arg(QString::fromUtf8(pa_strerror(error))));
        }
        return;
    }

    std::vector<int16_t> buffer(kFramesPerBlock * kChannels);

    while (!m_stop.load(std::memory_order_relaxed)) {
        const int result = pa_simple_read(
            handle, buffer.data(), buffer.size() * sizeof(int16_t), &error);
        if (result < 0) {
            if (!m_stop.load(std::memory_order_relaxed) && m_errorCallback) {
                m_errorCallback(QStringLiteral("系统声音采集失败：%1")
                                    .arg(QString::fromUtf8(pa_strerror(error))));
            }
            break;
        }

        if (m_pcmCallback != nullptr) {
            m_pcmCallback(buffer.data(),
                          buffer.size() / static_cast<size_t>(spec.channels),
                          spec.rate,
                          static_cast<size_t>(spec.channels));
        }
    }

    pa_simple_free(handle);
    LOG_INFO("ScreenAudio") << "系统声音采集已停止";
}
