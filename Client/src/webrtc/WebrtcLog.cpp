#include "webrtc/WebrtcLog.h"

#include "core/Log.h"

#include "rtc_base/logging.h"

namespace {

// WebRTC 内部日志统一记为 Trace：默认 info 级别完全不输出，只有把
// SYNCINE_LOG_LEVEL 调到 trace（或设置页选 trace）时才看得到。
// 这样不再需要维护一长串“哪些消息算噪声”的过滤表，排查时也不会漏掉意外消息。
class Sink : public webrtc::LogSink {
public:
    // 带级别的那一版才是实际入口：分发时调的就是它（logging.cc:286，每个 sink
    // 只调一次），消息、级别都在手里，不必再自己拼前缀。
    void OnLogMessage(const webrtc::LogLineRef &line) override
    {
        // trace 没开时直接返回，连 UTF-8 → QString 的转换都省掉。
        if (!Log::isEnabled(Log::Level::Trace))
            return;

        const absl::string_view message = line.message();
        Log::write(Log::Level::Trace, "WebRTC",
                   QString::fromUtf8(message.data(), static_cast<int>(message.size())));
    }

    // 纯虚函数，必须实现（否则这个类是抽象类，编译不过）。
    // 正常情况下到不了这里；级别信息在 string 形态里已经丢失，也只记 Trace。
    void OnLogMessage(const std::string &message) override
    {
        if (!Log::isEnabled(Log::Level::Trace))
            return;
        Log::write(Log::Level::Trace, "WebRTC", QString::fromStdString(message));
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
    // 不设置 verbose 时接 INFO 及以上（WebRTC 的默认工作级别）；设置了
    // SYNCINE_WEBRTC_LOG 才连 VERBOSE 一起接。无论级别如何，写入日志时
    // 都统一标成 Trace，默认 info 下不会输出。
    webrtc::LogMessage::AddLogToStream(&g_sink, verbose ? webrtc::LS_VERBOSE
                                                        : webrtc::LS_INFO);
}
