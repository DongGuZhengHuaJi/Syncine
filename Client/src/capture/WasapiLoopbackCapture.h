//
// Windows 系统声音采集：WASAPI loopback。
//
// 首选进程回环（ActivateAudioInterfaceAsync +
// PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE），只采集其他进程的
// 声音，排除本软件自己播放的远端语音；进程回环不可用时默认关闭系统声音
// 采集（而不是回退整机 loopback，避免永久回声），可用环境变量
// SYNCINE_SCREEN_AUDIO_ALLOW_FULL_LOOPBACK=1 强制回退。
//
// 输出统一为 48k/2ch/Int16。进程回环不支持 GetMixFormat，必须显式给
// WAVEFORMATEX；整机 loopback 仍走 GetMixFormat。
//

#ifndef SYNCINE_WASAPILOOPBACKCAPTURE_H
#define SYNCINE_WASAPILOOPBACKCAPTURE_H

#include <atomic>
#include <thread>

#include "SystemAudioCapture.h"

class WasapiLoopbackCapture : public SystemAudioCapture {
public:
    WasapiLoopbackCapture() = default;
    ~WasapiLoopbackCapture() override;

    bool start(const QString &outputDeviceName,
               PcmCallback pcmCallback,
               ErrorCallback errorCallback) override;
    void stop() override;

    // true = 本次采集使用进程回环（排除本进程树）；false = 回退整机 loopback。
    bool isolatesOwnAudio() const override
    {
        return m_processLoopback.load(std::memory_order_relaxed);
    }

    static bool isSupported();

private:
    void threadLoop(QString endpointId);

    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_processLoopback{false};
    PcmCallback m_pcmCallback;
    ErrorCallback m_errorCallback;
};

#endif //SYNCINE_WASAPILOOPBACKCAPTURE_H
