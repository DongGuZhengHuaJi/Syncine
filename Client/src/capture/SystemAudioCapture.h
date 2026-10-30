//
// 系统声音采集的抽象接口。
//
// 输入：当前「听筒」对应的输出设备的描述名（与 AppSettings.speakerDevice()
// 同源）；空串表示系统默认输出。
// 输出：48000 Hz / 2 声道 / Int16 交错 PCM，通过回调交给上层。
//
// 回调在采集线程执行；上层（ScreenShareController）只做转发，
// 真正的消费者 MovieAudioSource 自带锁，可以在该线程直接调用。
//
// 平台实现：
//   Linux   → PulseMonitorCapture（libpulse 的 monitor source）
//   Windows → WasapiLoopbackCapture（WASAPI loopback）
//   其他    → 工厂返回 nullptr，界面显示不可用
//

#ifndef SYNCINE_SYSTEMAUDIOCAPTURE_H
#define SYNCINE_SYSTEMAUDIOCAPTURE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <QString>

class SystemAudioCapture {
public:
    using PcmCallback = std::function<void(const int16_t *data,
                                           size_t samplesPerChannel,
                                           int sampleRate,
                                           size_t channels)>;
    using ErrorCallback = std::function<void(const QString &message)>;

    virtual ~SystemAudioCapture() = default;

    // 返回 false 表示立即失败；异步失败通过 errorCallback 报告。
    virtual bool start(const QString &outputDeviceName,
                       PcmCallback pcmCallback,
                       ErrorCallback errorCallback) = 0;
    virtual void stop() = 0;

    // 当前构建/平台是否提供系统声音采集。
    static bool isSupported();

    // 调用方负责 unique_ptr 生命周期；不支持时返回 nullptr。
    static std::unique_ptr<SystemAudioCapture> create();

    // 当前实现是否已把本进程自身的声音排除在采集之外：
    //   Windows 进程回环（排除自身进程树）→ true
    //   Linux 虚拟 Sink + 流路由            → true
    //   整机 loopback / monitor 回退        → false
    // 只用于日志和界面提示，不影响采集本身。
    virtual bool isolatesOwnAudio() const { return false; }
};

#endif //SYNCINE_SYSTEMAUDIOCAPTURE_H
