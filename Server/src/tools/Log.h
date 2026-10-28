#ifndef SERVER_LOG_H
#define SERVER_LOG_H

#include <memory>
#include <sstream>
#include <string>

// 服务端日志：控制台 + 本地文件，行格式与客户端 (Client/src/core/Log.h) 一致，
// 联调时两边可直接按时间戳对照：
//
//     2026-10-10 14:32:05.123 [I] [Logic     ] [t1] 房间创建: 123456
//
// 与客户端为两份独立实现（服务端不依赖 Qt），仅共享级别、行格式与环境变量约定：
// SYNCINE_LOG_LEVEL / SYNCINE_LOG_DIR。
namespace Log {

enum class Level : int { Trace = 0, Debug, Info, Warn, Error, Fatal };

// 打开日志文件。应在 main() 起始处调用，越早越好。
void init(const std::string &filePrefix = "server");
void shutdown();

void setLevel(Level level);
Level level();
bool isEnabled(Level level);

void nameCurrentThread(const char *name);

std::string filePath();

void write(Level level, const char *tag, const std::string &message);

// 流式一行，见文件头说明。被过滤掉的行不做字符串拼接。
class Line {
public:
    Line(Level level, const char *tag);

    ~Line();

    template <typename T>
    Line &operator<<(const T &value)
    {
        if (m_active && m_os)
            *m_os << value;
        return *this;
    }

private:
    bool m_active = false;
    Level m_level = Level::Info;
    const char *m_tag = "";
    // 被过滤掉的行不构造流，避免只为丢弃而拼接字符串。
    std::unique_ptr<std::ostringstream> m_os;
};

} // namespace Log

#define LOG_TRACE(tag) ::Log::Line(::Log::Level::Trace, tag)
#define LOG_DEBUG(tag) ::Log::Line(::Log::Level::Debug, tag)
#define LOG_INFO(tag) ::Log::Line(::Log::Level::Info, tag)
#define LOG_WARN(tag) ::Log::Line(::Log::Level::Warn, tag)
#define LOG_ERROR(tag) ::Log::Line(::Log::Level::Error, tag)
#define LOG_FATAL(tag) ::Log::Line(::Log::Level::Fatal, tag)

#endif // SERVER_LOG_H
