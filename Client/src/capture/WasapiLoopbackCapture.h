//
// Windows 系统声音采集：WASAPI loopback。
//
// 采集默认输出设备（或按 AppSettings 的听筒描述匹配到的 endpoint）的
// 混音输出，转换为 48k/2ch/Int16。当前版本是整机 loopback，
// 会包含本软件自己播放的远端语音；进程回环（排除自身）留待后续。
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
