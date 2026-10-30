//
// Linux 系统声音采集：libpulse 的 monitor source。
//
// Qt 的 QMediaDevices::audioInputs() 与 libwebrtc ADM 都会过滤 monitor，
// 所以这里直接走 libpulse 的 simple API。采集线程里 pa_simple_read()
// 以 20ms 为一块读取，停止时等当前一块返回后退出。
//

#ifndef SYNCINE_PULSEMONITORCAPTURE_H
#define SYNCINE_PULSEMONITORCAPTURE_H

#include <atomic>
#include <memory>
#include <thread>

#include "SystemAudioCapture.h"

class PulseSinkRouter;

class PulseMonitorCapture : public SystemAudioCapture {
public:
    PulseMonitorCapture();
    ~PulseMonitorCapture() override;

    bool start(const QString &outputDeviceName,
               PcmCallback pcmCallback,
               ErrorCallback errorCallback) override;
    void stop() override;

    bool isolatesOwnAudio() const override { return m_isolated; }

    static bool isSupported();

private:
    void threadLoop(QString monitorSource);

    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    PcmCallback m_pcmCallback;
    ErrorCallback m_errorCallback;

    // 采集期间存活；负责虚拟 Sink、回放 loopback 与其他应用流的路由。
    std::unique_ptr<PulseSinkRouter> m_router;
    bool m_isolated = false;
};

#endif //SYNCINE_PULSEMONITORCAPTURE_H
