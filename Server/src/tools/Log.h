#ifndef SERVER_LOG_H
#define SERVER_LOG_H

#include <sstream>
#include <string>

// 服务端日志:控制台 + 本地文件,格式与客户端 (Client/src/core/Log.h) 完全一致,
// 联调时两边日志可以按时间戳直接对着看:
//
//     2026-10-10 14:32:05.123 [I] [Logic     ] [t1] 房间创建: 123456
//
// 用法:
//     LOG_INFO("Logic") << "房间创建: " << roomId;
//     LOG_WARN("Session") << "Write failed: " << ec.message();
//
// 和客户端那版是**两份独立实现**,不是共享代码:客户端那份基于 Qt
// (QString/QMutex/QStandardPaths),服务端这边不依赖 Qt。两边只共享"设计":
// 同样的级别、同样的行格式、同样的环境变量(SYNCINE_LOG_LEVEL / SYNCINE_LOG_DIR)。
namespace Log {

enum class Level : int { Trace = 0, Debug, Info, Warn, Error, Fatal };

// 打开日志文件。main() 第一件事就该调,越早越好。
void init(const std::string &filePrefix = "server");
void shutdown();

void setLevel(Level level);
Level level();
bool isEnabled(Level level);

void nameCurrentThread(const char *name);

std::string filePath();

void write(Level level, const char *tag, const std::string &message);

// 流式一行,见文件头的说明。被过滤掉的行连字符串都不拼。
class Line {
public:
    Line(Level level, const char *tag);

    ~Line();

    template <typename T>
    Line &operator<<(const T &value)
    {
        if (m_active)
            m_os << value;
        return *this;
    }

private:
    bool m_active = false;
    Level m_level = Level::Info;
    const char *m_tag = "";
    // 和客户端不同,这里不需要"提前析构再取内容"那一套:
    // ostringstream 是边写边拼的,析构函数里随时能读到完整内容。
    std::ostringstream m_os;
};

} // namespace Log

#define LOG_TRACE(tag) ::Log::Line(::Log::Level::Trace, tag)
#define LOG_DEBUG(tag) ::Log::Line(::Log::Level::Debug, tag)
#define LOG_INFO(tag) ::Log::Line(::Log::Level::Info, tag)
#define LOG_WARN(tag) ::Log::Line(::Log::Level::Warn, tag)
#define LOG_ERROR(tag) ::Log::Line(::Log::Level::Error, tag)
#define LOG_FATAL(tag) ::Log::Line(::Log::Level::Fatal, tag)

#endif // SERVER_LOG_H
