//
// Linux：为系统声音采集建立「隔离用虚拟 Sink」。
//
// 背景：PulseAudio / PipeWire 的 monitor source 只能按 Sink 采集，不能
// 按应用排除。直接采真实 Sink 的 monitor，会把 Syncine 自己播放的房间
// 语音也采回去，形成回声。
//
// 本类通过 pipewire-pulse / PulseAudio 的模块 API 做三件事：
//   1. module-null-sink：创建一个虚拟 Sink（在 PipeWire 图里就是一个节点）；
//   2. module-loopback：把虚拟 Sink 的 monitor 回放到真实 Sink，
//      用户仍能听见被采集的应用声音；
//   3. server-side 流路由：把其他应用的播放流移到虚拟 Sink；
//      Syncine 自己的播放流（按进程 PID 识别）留在真实 Sink。
// 这样采集虚拟 Sink 的 monitor 时，里面只有其他应用的声音，没有房间语音。
//
// 停止时把流移回原 Sink，并卸载 loopback / null-sink 模块。
// 任何一步失败都由调用方回退到整机 monitor 采集。
//
// 只依赖 libpulse。在 PipeWire 系统上由 pipewire-pulse 执行模块；
// 普通 PulseAudio 同样适用。
//

#ifndef SYNCINE_PULSESINKROUTER_H
#define SYNCINE_PULSESINKROUTER_H

#include <QString>

#include <memory>

class PulseSinkRouter {
public:
    PulseSinkRouter();
    ~PulseSinkRouter();

    PulseSinkRouter(const PulseSinkRouter &) = delete;
    PulseSinkRouter &operator=(const PulseSinkRouter &) = delete;

    // realSinkName 是 Pulse Sink 名（不是设备描述）；空串表示服务端默认。
    // 成功时 monitorSource 返回虚拟 Sink 的 monitor source 名。
    bool start(const QString &realSinkName, QString *monitorSource,
               QString *error);
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif // SYNCINE_PULSESINKROUTER_H
