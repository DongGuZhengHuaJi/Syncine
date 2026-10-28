#include "webrtc/WebrtcLog.h"

#include "core/Log.h"

#include "rtc_base/logging.h"

namespace {

// 级别映射。Trace/Debug 走 verbose 那档 —— WebRTC 的 VERBOSE 就是它的最细一级，
// 与本项目的 trace 语义一致（LS_VERBOSE 之上没有更细的级别了）。
Log::Level levelOf(webrtc::LoggingSeverity severity)
{
    switch (severity) {
    case webrtc::LS_VERBOSE: return Log::Level::Trace;
    case webrtc::LS_INFO:    return Log::Level::Info;
    case webrtc::LS_WARNING: return Log::Level::Warn;
    case webrtc::LS_ERROR:   return Log::Level::Error;
    case webrtc::LS_NONE:    break;
    }
    return Log::Level::Error;
}

// WebRTC 里有一批“预期内但每帧/每秒都刷”的消息：哑声卡没有 playout delay、
// IPv4 STUN 服务器配 IPv6 host 候选、TURN 对私有 peer 地址的 403 等。
// 它们不是我们要处理的故障，按 Trace 记录：默认 Info 看不到，开 trace 仍可查。
bool isNoisy(absl::string_view message)
{
    static const char *patterns[] = {
        "failed to retrieve the playout delay",
        "Unable to set playout device",
        "Failed to initialize recording",
        "Can't query latency",
        "STUN server address is incompatible",
        "TURN allocate request",
        "CreatePermission error response",
        "create permission error response",
        "Not adding candidate because the JsepTransport",
        "Discarding candidate because port is already done gathering",
        "COM already initialized with a different apartment model",
        "AudioDeviceWindowsCore::~AudioDeviceWindowsCore",
        "Will not log any `WebRTC.Audio",
        "DataChannelObserver - adapter needed",
    };
    for (const char *pattern : patterns) {
        if (message.find(pattern) != absl::string_view::npos)
            return true;
    }
    return false;
}

class Sink : public webrtc::LogSink {
public:
    // 带级别的那一版才是实际入口：分发时调的就是它（logging.cc:286，每个 sink
    // 只调一次），消息、级别都在手里，不必再自己拼前缀。
    void OnLogMessage(const webrtc::LogLineRef &line) override
    {
        const absl::string_view message = line.message();
        const Log::Level level = isNoisy(message) ? Log::Level::Trace
                                                   : levelOf(line.severity());
        Log::write(level, "WebRTC",
                   QString::fromUtf8(message.data(), static_cast<int>(message.size())));
    }

    // 纯虚函数，必须实现（否则这个类是抽象类，编译不过）。
    //
    // 实际上到不了这里 —— LogSink 默认实现的那条链（LogLineRef → 带级别的
    // string_view/string 重载）在上一版就被我们截断了，而级别信息在 string
    // 形态里已经丢失，只能按 info 记。留作兜底，正常情况下不会出现在日志里。
    void OnLogMessage(const std::string &message) override
    {
        Log::write(Log::Level::Info, "WebRTC", QString::fromStdString(message));
    }
};

// 静态存储期：WebRTC 只存裸指针，这个对象必须活得比它还久。
Sink g_sink;

} // namespace

void WebrtcLog::install(bool verbose)
{
    // 只调一次：AddLogToStream 是把 sink 头插进单向链表（logging.cc:355），
    // 没有去重，同一个 sink 注册两次会把自己串成一个环。
    //
    // 接在警告档时开销极低：低于最低级别的 RTC_LOG 会在拼字符串之前就短路
    // （LogMessage::IsNoop()，logging.cc:524），连参数都不会被格式化。
    webrtc::LogMessage::AddLogToStream(&g_sink, verbose ? webrtc::LS_INFO
                                                        : webrtc::LS_WARNING);
}
