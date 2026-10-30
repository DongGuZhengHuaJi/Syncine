//
// Linux 系统声音采集的「虚拟 Sink + 流路由」实现。
//
// 详细背景见头文件。这里只强调几个实现约束：
//   · 所有 Pulse 调用都在 pa_threaded_mainloop 的锁内；等待操作用
//     pa_threaded_mainloop_wait()，由回调 signal 唤醒。
//   · 订阅回调在 mainloop 线程执行；stop() 会先断开 context 再停 mainloop，
//     因此回调不会在 Impl 销毁后访问它。
//   · 「自己人」按进程 PID 识别。WebRTC ADM 的 Pulse client 是
//     "WEBRTC VoiceEngine"，Qt 的播放 client 是 "Syncine"；两者的
//     application.process.id 都是本进程。ADM 的 sink-input 直接带 PID，
//     Qt 的 sink-input 有时没有 proplist，故还必须维护 client 索引集合。
//   · module-loopback 自己的 sink-input 用 owner_module 排除，避免把
//     回放流又搬进虚拟 Sink 形成环路。
//

#include "PulseSinkRouter.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

#include <pulse/pulseaudio.h>

#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>

#include "core/Log.h"

namespace {

QString pulseError(const pa_context *context)
{
    if (context == nullptr)
        return QStringLiteral("PulseAudio 上下文未建立");
    return QString::fromUtf8(pa_strerror(pa_context_errno(context)));
}

QByteArray pidOfClient(const pa_client_info *info)
{
    if (info == nullptr || info->proplist == nullptr)
        return {};
    const char *pid = pa_proplist_gets(info->proplist,
                                       PA_PROP_APPLICATION_PROCESS_ID);
    return pid != nullptr ? QByteArray(pid) : QByteArray();
}

// 从 "sink_name=syncine_share_<pid>_<ts>" / "source=syncine_share_<pid>_<ts>.monitor"
// 里取出创建者的 PID。用于清理上次异常退出留下的模块。
pid_t pidFromModuleArgument(const char *argument)
{
    if (argument == nullptr)
        return -1;

    const QByteArray text(argument);
    const int marker = text.indexOf("syncine_share_");
    if (marker < 0)
        return -1;

    const int start = marker + static_cast<int>(std::strlen("syncine_share_"));
    int end = start;
    while (end < text.size() && text.at(end) >= '0' && text.at(end) <= '9')
        ++end;
    if (end == start)
        return -1;

    bool ok = false;
    const uint32_t pid = text.mid(start, end - start).toUInt(&ok);
    return ok ? static_cast<pid_t>(pid) : -1;
}

bool processIsAlive(pid_t pid)
{
    if (pid <= 0)
        return true; // 解析不出来时保守处理：不动它
    if (kill(pid, 0) == 0)
        return true;
    return errno != ESRCH;
}

} // namespace

struct PulseSinkRouter::Impl {
    pa_threaded_mainloop *mainloop = nullptr;
    pa_context *context = nullptr;
    pa_context_state_t state = PA_CONTEXT_UNCONNECTED;

    uint32_t nullModule = PA_INVALID_INDEX;
    uint32_t loopbackModule = PA_INVALID_INDEX;
    uint32_t virtualSinkIndex = PA_INVALID_INDEX;
    uint32_t realSinkIndex = PA_INVALID_INDEX;

    QString virtualSinkName;

    // 被我方搬到虚拟 Sink 的流 → 原 Sink。停止时按这个表搬回去。
    QHash<uint32_t, uint32_t> movedFrom;
    // 本进程的 Pulse client 索引。sink-input 的 client 命中即为「自己人」。
    QSet<uint32_t> selfClients;

    // 同步等待用的临时结果。所有操作串行执行，故一组标志足够。
    bool lastSuccess = false;
    bool stopping = false;
    uint32_t lastModule = PA_INVALID_INDEX;
    uint32_t lastSinkIndex = PA_INVALID_INDEX;
    QString lastDefaultSink;

    struct MoveTarget {
        uint32_t index = PA_INVALID_INDEX;
        uint32_t sink = PA_INVALID_INDEX;
    };
    QVector<MoveTarget> pendingMoves;
    QVector<uint32_t> staleLoopbacks;
    QVector<uint32_t> staleNulls;

    ~Impl() { stop(); }

    bool start(const QString &realSinkName, QString *monitorSource,
               QString *error);

    bool startLocked(const QString &realSinkName, QString *monitorSource,
                     QString *error);
    void stop();
    void stopLocked();

    // ---- 同步等待 ----
    bool waitForOperation(pa_operation *operation);
    bool connectContext(QString *error);
    bool loadModule(const char *name, const QString &arguments,
                    uint32_t *moduleIndex, QString *error);
    bool unloadModule(uint32_t moduleIndex);
    bool fetchDefaultSink(QString *sink);
    bool fetchSinkIndex(const QString &sinkName, uint32_t *sinkIndex);
    void cleanupStaleModules();
    void fetchSelfClients();
    void moveExistingStreams();
    bool moveSinkInputToVirtual(uint32_t index);
    void moveSinkInputToReal(uint32_t index);

    // ---- 事件处理 ----
    void onSinkInputNew(uint32_t index);
    void onSinkInputChanged(uint32_t index);
    void onClientNew(uint32_t index);
    bool isSelf(const pa_sink_input_info *info) const;
    bool isLoopback(const pa_sink_input_info *info) const;

    // ---- 回调 ----
    static void contextStateCallback(pa_context *context, void *userdata);
    static void moduleCallback(pa_context *context, uint32_t index,
                               void *userdata);
    static void successCallback(pa_context *context, int success,
                                void *userdata);
    static void serverInfoCallback(pa_context *context,
                                   const pa_server_info *info, void *userdata);
    static void sinkInfoCallback(pa_context *context, const pa_sink_info *info,
                                 int eol, void *userdata);
    static void clientListCallback(pa_context *context,
                                   const pa_client_info *info, int eol,
                                   void *userdata);
    static void clientInfoCallback(pa_context *context,
                                   const pa_client_info *info, int eol,
                                   void *userdata);
    static void sinkInputListCallback(pa_context *context,
                                      const pa_sink_input_info *info, int eol,
                                      void *userdata);
    static void newSinkInputInfoCallback(pa_context *context,
                                         const pa_sink_input_info *info,
                                         int eol, void *userdata);
    static void changedSinkInputInfoCallback(pa_context *context,
                                             const pa_sink_input_info *info,
                                             int eol, void *userdata);
    static void subscribeCallback(pa_context *context,
                                  pa_subscription_event_type_t type,
                                  uint32_t index, void *userdata);
    static void moduleListCallback(pa_context *context,
                                   const pa_module_info *info, int eol,
                                   void *userdata);
};

// ============================
// 生命周期
// ============================

PulseSinkRouter::PulseSinkRouter() : m_impl(std::make_unique<Impl>()) {}

PulseSinkRouter::~PulseSinkRouter() = default;

bool PulseSinkRouter::start(const QString &realSinkName,
                            QString *monitorSource, QString *error)
{
    if (!m_impl->start(realSinkName, monitorSource, error)) {
        // start() 失败时内部已经清理；把 Impl 一起收掉，避免半成品状态。
        m_impl = std::make_unique<Impl>();
        return false;
    }
    return true;
}

void PulseSinkRouter::stop()
{
    m_impl->stop();
}

bool PulseSinkRouter::Impl::start(const QString &realSinkName,
                                  QString *monitorSource, QString *error)
{
    mainloop = pa_threaded_mainloop_new();
    if (mainloop == nullptr) {
        if (error)
            *error = QStringLiteral("创建 PulseAudio mainloop 失败");
        return false;
    }

    if (pa_threaded_mainloop_start(mainloop) < 0) {
        if (error)
            *error = QStringLiteral("启动 PulseAudio mainloop 失败");
        pa_threaded_mainloop_free(mainloop);
        mainloop = nullptr;
        return false;
    }

    pa_threaded_mainloop_lock(mainloop);
    const bool ok = startLocked(realSinkName, monitorSource, error);
    pa_threaded_mainloop_unlock(mainloop);

    if (!ok)
        stop();
    return ok;
}

bool PulseSinkRouter::Impl::startLocked(const QString &realSinkName,
                                        QString *monitorSource, QString *error)
{
    if (!connectContext(error))
        return false;
    stopping = false;

    // 上次异常退出可能留下空 Sink / loopback 模块（进程死了没人卸载）。
    cleanupStaleModules();

    QString targetSink = realSinkName;
    if (targetSink.isEmpty()) {
        if (!fetchDefaultSink(&targetSink) || targetSink.isEmpty()) {
            if (error)
                *error = QStringLiteral("找不到默认输出 Sink");
            return false;
        }
    }
    if (!fetchSinkIndex(targetSink, &realSinkIndex)) {
        if (error)
            *error = QStringLiteral("找不到真实输出 Sink：%1").arg(targetSink);
        return false;
    }

    // 名字必须全局唯一：多个 Syncine 实例、或上一次异常退出残留的 Sink
    // 都可能同名。PID + 毫秒时间戳足够区分。
    virtualSinkName = QStringLiteral("syncine_share_%1_%2")
                          .arg(static_cast<qulonglong>(getpid()))
                          .arg(QDateTime::currentMSecsSinceEpoch());

    const QString nullSinkArgs =
        QStringLiteral("sink_name=%1 rate=48000 channels=2 "
                       "sink_properties=device.description=Syncine_Share_Capture")
            .arg(virtualSinkName);
    if (!loadModule("module-null-sink", nullSinkArgs, &nullModule, error))
        return false;

    if (!fetchSinkIndex(virtualSinkName, &virtualSinkIndex)) {
        if (error)
            *error = QStringLiteral("创建虚拟 Sink 后找不到它");
        return false;
    }

    const QString loopbackArgs =
        QStringLiteral("source=%1.monitor sink=%2 latency_msec=20 "
                       "source_dont_move=true sink_dont_move=true")
            .arg(virtualSinkName, targetSink);
    if (!loadModule("module-loopback", loopbackArgs, &loopbackModule, error))
        return false;

    // 订阅一定要在移动现有流之前建立，否则这中间新建的流会漏掉。
    pa_context_set_subscribe_callback(context, subscribeCallback, this);
    lastSuccess = false;
    pa_operation *subscribeOp = pa_context_subscribe(
        context,
        static_cast<pa_subscription_mask_t>(
            PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_CLIENT),
        successCallback, this);
    if (!waitForOperation(subscribeOp) || !lastSuccess) {
        if (error)
            *error = QStringLiteral("订阅 PulseAudio 事件失败：%1")
                         .arg(pulseError(context));
        return false;
    }

    fetchSelfClients();
    moveExistingStreams();

    if (monitorSource != nullptr)
        *monitorSource = virtualSinkName + QStringLiteral(".monitor");

    LOG_INFO("ScreenAudio")
        << "系统声音已隔离：虚拟 Sink" << virtualSinkName << "→ 回放至"
        << targetSink << "，已接管" << movedFrom.size() << "个应用流";
    return true;
}

void PulseSinkRouter::Impl::stop()
{
    if (mainloop == nullptr)
        return;

    pa_threaded_mainloop_lock(mainloop);
    stopLocked();
    pa_threaded_mainloop_unlock(mainloop);

    pa_threaded_mainloop_stop(mainloop);
    pa_threaded_mainloop_free(mainloop);
    mainloop = nullptr;
}

void PulseSinkRouter::Impl::stopLocked()
{
    // 先置停止标志：已经排队的 NEW/CHANGE 查询回调不允许再往虚拟 Sink 搬流。
    stopping = true;
    if (context != nullptr) {
        if (state == PA_CONTEXT_READY) {
            // 先退订，避免停止过程中又搬来新的流。
            lastSuccess = false;
            pa_operation *unsubscribeOp = pa_context_subscribe(
                context, PA_SUBSCRIPTION_MASK_NULL, successCallback, this);
            waitForOperation(unsubscribeOp);

            // 把接管过的流搬回原 Sink。流可能已经不存在，失败忽略。
            const QHash<uint32_t, uint32_t> pending = movedFrom;
            movedFrom.clear();
            for (auto it = pending.constBegin(); it != pending.constEnd(); ++it) {
                lastSuccess = false;
                pa_operation *moveOp = pa_context_move_sink_input_by_index(
                    context, it.key(), it.value(), successCallback, this);
                if (!waitForOperation(moveOp) || !lastSuccess) {
                    LOG_DEBUG("ScreenAudio")
                        << "恢复流" << it.key() << "到 Sink" << it.value()
                        << "失败（流可能已消失）";
                }
            }

            // 先拆 loopback，再拆 null sink。
            if (loopbackModule != PA_INVALID_INDEX) {
                unloadModule(loopbackModule);
                loopbackModule = PA_INVALID_INDEX;
            }
            if (nullModule != PA_INVALID_INDEX) {
                unloadModule(nullModule);
                nullModule = PA_INVALID_INDEX;
            }
        }

        pa_context_disconnect(context);
        pa_context_unref(context);
        context = nullptr;
    }

    state = PA_CONTEXT_UNCONNECTED;
    virtualSinkIndex = PA_INVALID_INDEX;
    realSinkIndex = PA_INVALID_INDEX;
    virtualSinkName.clear();
    selfClients.clear();
    pendingMoves.clear();
    staleLoopbacks.clear();
    staleNulls.clear();
    lastModule = PA_INVALID_INDEX;
    lastSinkIndex = PA_INVALID_INDEX;
    lastDefaultSink.clear();
    lastSuccess = false;
    stopping = false;
}

// ============================
// 同步操作
// ============================

bool PulseSinkRouter::Impl::waitForOperation(pa_operation *operation)
{
    if (operation == nullptr)
        return false;

    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
        pa_threaded_mainloop_wait(mainloop);

    pa_operation_unref(operation);
    return true;
}

bool PulseSinkRouter::Impl::connectContext(QString *error)
{
    context = pa_context_new(pa_threaded_mainloop_get_api(mainloop),
                             "Syncine Screen Audio");
    if (context == nullptr) {
        if (error)
            *error = QStringLiteral("创建 PulseAudio 上下文失败");
        return false;
    }

    pa_context_set_state_callback(context, contextStateCallback, this);
    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
        if (error)
            *error = QStringLiteral("连接 PulseAudio 失败：%1")
                         .arg(pulseError(context));
        return false;
    }

    while (state != PA_CONTEXT_READY) {
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            if (error)
                *error = QStringLiteral("PulseAudio 连接失败：%1")
                             .arg(pulseError(context));
            return false;
        }
        pa_threaded_mainloop_wait(mainloop);
    }
    return true;
}

bool PulseSinkRouter::Impl::loadModule(const char *name,
                                       const QString &arguments,
                                       uint32_t *moduleIndex, QString *error)
{
    lastModule = PA_INVALID_INDEX;
    lastSuccess = false;
    const QByteArray args = arguments.toUtf8();
    pa_operation *operation =
        pa_context_load_module(context, name, args.constData(), moduleCallback,
                               this);
    if (!waitForOperation(operation) || !lastSuccess
        || lastModule == PA_INVALID_INDEX) {
        if (error)
            *error = QStringLiteral("加载 %1 失败：%2")
                         .arg(QString::fromLatin1(name), pulseError(context));
        lastModule = PA_INVALID_INDEX;
        return false;
    }
    *moduleIndex = lastModule;
    return true;
}

bool PulseSinkRouter::Impl::unloadModule(uint32_t moduleIndex)
{
    if (moduleIndex == PA_INVALID_INDEX || context == nullptr
        || state != PA_CONTEXT_READY) {
        return false;
    }

    lastSuccess = false;
    pa_operation *operation =
        pa_context_unload_module(context, moduleIndex, successCallback, this);
    return waitForOperation(operation) && lastSuccess;
}

bool PulseSinkRouter::Impl::fetchDefaultSink(QString *sink)
{
    lastDefaultSink.clear();
    pa_operation *operation =
        pa_context_get_server_info(context, serverInfoCallback, this);
    if (!waitForOperation(operation))
        return false;
    *sink = lastDefaultSink;
    return !sink->isEmpty();
}

bool PulseSinkRouter::Impl::fetchSinkIndex(const QString &sinkName,
                                           uint32_t *sinkIndex)
{
    lastSinkIndex = PA_INVALID_INDEX;
    const QByteArray name = sinkName.toUtf8();
    pa_operation *operation = pa_context_get_sink_info_by_name(
        context, name.constData(), sinkInfoCallback, this);
    if (!waitForOperation(operation))
        return false;
    *sinkIndex = lastSinkIndex;
    return lastSinkIndex != PA_INVALID_INDEX;
}

void PulseSinkRouter::Impl::cleanupStaleModules()
{
    staleLoopbacks.clear();
    staleNulls.clear();

    pa_operation *operation =
        pa_context_get_module_info_list(context, moduleListCallback, this);
    waitForOperation(operation);

    // 先拆 loopback，再拆空 Sink；顺序反了会留下指向已卸载 Sink 的 loopback。
    for (uint32_t index : staleLoopbacks)
        unloadModule(index);
    for (uint32_t index : staleNulls)
        unloadModule(index);

    if (!staleLoopbacks.isEmpty() || !staleNulls.isEmpty()) {
        LOG_INFO("ScreenAudio") << "清理上次残留的虚拟 Sink 模块："
                                << staleLoopbacks.size() << "个 loopback，"
                                << staleNulls.size() << "个 null-sink";
    }
    staleLoopbacks.clear();
    staleNulls.clear();
}

void PulseSinkRouter::Impl::fetchSelfClients()
{
    selfClients.clear();
    pa_operation *operation =
        pa_context_get_client_info_list(context, clientListCallback, this);
    waitForOperation(operation);
}

void PulseSinkRouter::Impl::moveExistingStreams()
{
    pendingMoves.clear();
    pa_operation *operation = pa_context_get_sink_input_info_list(
        context, sinkInputListCallback, this);
    waitForOperation(operation);

    const QVector<MoveTarget> targets = pendingMoves;
    pendingMoves.clear();
    for (const MoveTarget &target : targets) {
        movedFrom.insert(target.index, target.sink);
        if (!moveSinkInputToVirtual(target.index)) {
            movedFrom.remove(target.index);
            LOG_DEBUG("ScreenAudio")
                << "移动应用流" << target.index << "到虚拟 Sink 失败";
        } else {
            LOG_DEBUG("ScreenAudio")
                << "移动应用流" << target.index << "（原 Sink"
                << target.sink << "）到虚拟 Sink";
        }
    }
}

bool PulseSinkRouter::Impl::moveSinkInputToVirtual(uint32_t index)
{
    lastSuccess = false;
    // 用索引而不是名字：pipewire-pulse 对 by-name 的新建 null sink 会返回
    // EINVAL（实测），by-index 正常。
    pa_operation *operation = pa_context_move_sink_input_by_index(
        context, index, virtualSinkIndex, successCallback, this);
    const bool ok = waitForOperation(operation) && lastSuccess;
    if (!ok) {
        LOG_DEBUG("ScreenAudio") << "移动流" << index << "失败:"
                                << pulseError(context);
    }
    return ok;
}

void PulseSinkRouter::Impl::moveSinkInputToReal(uint32_t index)
{
    if (context == nullptr || state != PA_CONTEXT_READY
        || realSinkIndex == PA_INVALID_INDEX) {
        return;
    }
    pa_context_move_sink_input_by_index(context, index, realSinkIndex,
                                        nullptr, nullptr);
}

// ============================
// 事件处理
// ============================

void PulseSinkRouter::Impl::onSinkInputNew(uint32_t index)
{
    if (stopping || context == nullptr || state != PA_CONTEXT_READY)
        return;
    LOG_DEBUG("ScreenAudio") << "订阅：新流" << index;
    pa_operation *operation = pa_context_get_sink_input_info(
        context, index, newSinkInputInfoCallback, this);
    if (operation != nullptr)
        pa_operation_unref(operation);
}

void PulseSinkRouter::Impl::onSinkInputChanged(uint32_t index)
{
    if (stopping || context == nullptr || state != PA_CONTEXT_READY)
        return;
    pa_operation *operation = pa_context_get_sink_input_info(
        context, index, changedSinkInputInfoCallback, this);
    if (operation != nullptr)
        pa_operation_unref(operation);
}

void PulseSinkRouter::Impl::onClientNew(uint32_t index)
{
    if (stopping || context == nullptr || state != PA_CONTEXT_READY)
        return;
    pa_operation *operation = pa_context_get_client_info(
        context, index, clientInfoCallback, this);
    if (operation != nullptr)
        pa_operation_unref(operation);
}

bool PulseSinkRouter::Impl::isSelf(const pa_sink_input_info *info) const
{
    if (info == nullptr)
        return false;

    if (info->client != PA_INVALID_INDEX && selfClients.contains(info->client))
        return true;

    const char *pid = info->proplist != nullptr
                          ? pa_proplist_gets(info->proplist,
                                             PA_PROP_APPLICATION_PROCESS_ID)
                          : nullptr;
    if (pid == nullptr)
        return false;

    bool ok = false;
    const uint32_t value = QByteArray(pid).toUInt(&ok);
    return ok && value == static_cast<uint32_t>(getpid());
}

bool PulseSinkRouter::Impl::isLoopback(const pa_sink_input_info *info) const
{
    return info != nullptr && loopbackModule != PA_INVALID_INDEX
           && info->owner_module == loopbackModule;
}

// ============================
// 回调
// ============================

void PulseSinkRouter::Impl::contextStateCallback(pa_context *context,
                                                  void *userdata)
{
    auto *self = static_cast<Impl *>(userdata);
    self->state = pa_context_get_state(context);
    pa_threaded_mainloop_signal(self->mainloop, 0);
}

void PulseSinkRouter::Impl::moduleCallback(pa_context *, uint32_t index,
                                            void *userdata)
{
    auto *self = static_cast<Impl *>(userdata);
    self->lastModule = index;
    self->lastSuccess = index != PA_INVALID_INDEX;
    pa_threaded_mainloop_signal(self->mainloop, 0);
}

void PulseSinkRouter::Impl::successCallback(pa_context *, int success,
                                             void *userdata)
{
    auto *self = static_cast<Impl *>(userdata);
    self->lastSuccess = success != 0;
    pa_threaded_mainloop_signal(self->mainloop, 0);
}

void PulseSinkRouter::Impl::serverInfoCallback(pa_context *,
                                                const pa_server_info *info,
                                                void *userdata)
{
    auto *self = static_cast<Impl *>(userdata);
    if (info != nullptr && info->default_sink_name != nullptr)
        self->lastDefaultSink = QString::fromUtf8(info->default_sink_name);
    pa_threaded_mainloop_signal(self->mainloop, 0);
}

void PulseSinkRouter::Impl::sinkInfoCallback(pa_context *,
                                              const pa_sink_info *info, int eol,
                                              void *userdata)
{
    auto *self = static_cast<Impl *>(userdata);
    if (info != nullptr && eol == 0)
        self->lastSinkIndex = info->index;
    if (eol != 0)
        pa_threaded_mainloop_signal(self->mainloop, 0);
}

void PulseSinkRouter::Impl::clientListCallback(pa_context *,
                                                const pa_client_info *info,
                                                int eol, void *userdata)
{
    auto *self = static_cast<Impl *>(userdata);
    if (info != nullptr) {
        const QByteArray pid = pidOfClient(info);
        if (!pid.isEmpty()
            && pid.toUInt() == static_cast<uint32_t>(getpid())) {
            self->selfClients.insert(info->index);
        }
    }
    if (eol != 0)
        pa_threaded_mainloop_signal(self->mainloop, 0);
}

void PulseSinkRouter::Impl::clientInfoCallback(pa_context *,
                                                const pa_client_info *info,
                                                int eol, void *userdata)
{
    auto *self = static_cast<Impl *>(userdata);
    if (info != nullptr) {
        const QByteArray pid = pidOfClient(info);
        if (!pid.isEmpty()
            && pid.toUInt() == static_cast<uint32_t>(getpid())) {
            self->selfClients.insert(info->index);
        }
    }
    if (eol != 0)
        pa_threaded_mainloop_signal(self->mainloop, 0);
}

void PulseSinkRouter::Impl::sinkInputListCallback(
    pa_context *, const pa_sink_input_info *info, int eol, void *userdata)
{
    auto *self = static_cast<Impl *>(userdata);
    if (info != nullptr && eol == 0) {
        LOG_DEBUG("ScreenAudio") << "初始流: index=" << info->index
                                << " client=" << info->client
                                << " sink=" << info->sink
                                << " self=" << (self->isSelf(info) ? 1 : 0)
                                << " loopback=" << (self->isLoopback(info) ? 1 : 0);
        if (!self->isSelf(info) && !self->isLoopback(info)
            && info->sink != self->virtualSinkIndex) {
            self->pendingMoves.append({info->index, info->sink});
        }
    }
    if (eol != 0)
        pa_threaded_mainloop_signal(self->mainloop, 0);
}

void PulseSinkRouter::Impl::newSinkInputInfoCallback(
    pa_context *, const pa_sink_input_info *info, int eol, void *userdata)
{
    if (info == nullptr || eol != 0)
        return;

    auto *self = static_cast<Impl *>(userdata);
    if (self->stopping || self->context == nullptr
        || self->state != PA_CONTEXT_READY) {
        return;
    }
    LOG_DEBUG("ScreenAudio") << "新流信息: index=" << info->index
                            << " client=" << info->client
                            << " sink=" << info->sink
                            << " self=" << (self->isSelf(info) ? 1 : 0)
                            << " loopback=" << (self->isLoopback(info) ? 1 : 0);
    if (self->isSelf(info) || self->isLoopback(info)
        || info->sink == self->virtualSinkIndex) {
        return;
    }

    self->movedFrom.insert(info->index, info->sink);
    pa_context_move_sink_input_by_index(self->context, info->index,
                                        self->virtualSinkIndex, nullptr,
                                        nullptr);
}

void PulseSinkRouter::Impl::changedSinkInputInfoCallback(
    pa_context *, const pa_sink_input_info *info, int eol, void *userdata)
{
    if (info == nullptr || eol != 0)
        return;

    auto *self = static_cast<Impl *>(userdata);
    if (self->stopping || self->context == nullptr
        || self->state != PA_CONTEXT_READY) {
        return;
    }

    if (info->sink != self->virtualSinkIndex) {
        // 用户（或应用自己）把流移走了：尊重这个选择，停止时不再搬回去。
        self->movedFrom.remove(info->index);
        return;
    }

    // 自己的流不应该出现在采集 Sink 里（唯一可能是路由竞态）。
    // 把它挪回真实 Sink，宁可短暂中断也不要把房间语音采回去。
    if (self->isSelf(info)) {
        self->movedFrom.remove(info->index);
        LOG_WARN("ScreenAudio") << "检测到自身播放流进入虚拟 Sink，移回真实 Sink";
        self->moveSinkInputToReal(info->index);
    }
}

void PulseSinkRouter::Impl::moduleListCallback(pa_context *,
                                               const pa_module_info *info,
                                               int eol, void *userdata)
{
    auto *self = static_cast<Impl *>(userdata);

    if (info != nullptr) {
        const QByteArray name =
            info->name != nullptr ? QByteArray(info->name) : QByteArray();
        const QByteArray argument =
            info->argument != nullptr ? QByteArray(info->argument) : QByteArray();

        if (argument.contains("syncine_share_")) {
            const pid_t pid = pidFromModuleArgument(info->argument);
            // 本进程的残留（上一次启动失败没卸干净）也可以清；此时我们
            // 还没创建自己的模块，不会误删正在使用的。
            if (pid == getpid() || !processIsAlive(pid)) {
                if (name == "module-loopback")
                    self->staleLoopbacks.append(info->index);
                else if (name == "module-null-sink")
                    self->staleNulls.append(info->index);
            }
        }
    }

    if (eol != 0)
        pa_threaded_mainloop_signal(self->mainloop, 0);
}

void PulseSinkRouter::Impl::subscribeCallback(
    pa_context *, pa_subscription_event_type_t type, uint32_t index,
    void *userdata)
{
    auto *self = static_cast<Impl *>(userdata);
    const auto facility = static_cast<pa_subscription_event_type_t>(
        type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK);
    const auto eventType = static_cast<pa_subscription_event_type_t>(
        type & PA_SUBSCRIPTION_EVENT_TYPE_MASK);

    if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        if (eventType == PA_SUBSCRIPTION_EVENT_REMOVE) {
            self->movedFrom.remove(index);
        } else if (eventType == PA_SUBSCRIPTION_EVENT_NEW) {
            self->onSinkInputNew(index);
        } else if (eventType == PA_SUBSCRIPTION_EVENT_CHANGE) {
            self->onSinkInputChanged(index);
        }
    } else if (facility == PA_SUBSCRIPTION_EVENT_CLIENT) {
        if (eventType == PA_SUBSCRIPTION_EVENT_REMOVE) {
            self->selfClients.remove(index);
        } else if (eventType == PA_SUBSCRIPTION_EVENT_NEW) {
            self->onClientNew(index);
        }
    }
}
