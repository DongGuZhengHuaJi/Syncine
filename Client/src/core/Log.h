#pragma once

#include <QByteArray>
#include <QDebug>
#include <QString>

#include <memory>
#include <string>

// 全局日志：同时写控制台和本地文件。
//
// 用法：LOG_INFO("WebRTC") << "已建立对端连接: " << peerId;
//
// init() 会安装 Qt 消息处理器，qDebug/qWarning 与 QML 的 console.log
// 一并收进同一文件，排查 QML 问题时无需另开终端。
namespace Log {

enum class Level : int { Trace = 0, Debug, Info, Warn, Error, Fatal };

// 打开日志文件、安装 Qt 消息处理器。应在 main() 中创建完 QGuiApplication 后
// 立即调用，以保证后续所有对象构造期的日志都能落进文件。
void init(const QString &filePrefix = QStringLiteral("syncine"));
void shutdown();

void setLevel(Level level);
Level level();
bool isEnabled(Level level);

// 给当前线程命名。每个线程在日志中显示为 [t1]/[t2]……，
// 命名后会多打一行说明，用于确认回调所在线程。
void nameCurrentThread(const char *name);

QString filePath();

// 直接写一行（一般不直接调用，使用上面的宏）
void write(Level level, const char *tag, const QString &message);

// 流式的一行。不要自行构造对象，使用 LOG_XXX 宏。
class Line {
public:
    Line(Level level, const char *tag);

    ~Line();

    // 先判级别再拼接：构造函数中已确定是否输出，被过滤的行不执行本运算符。
    // 否则 60fps 路径上的 LOG_TRACE 会把 CPU 耗在拼接字符串后丢弃。
    template <typename T>
    Line &operator<<(const T &value)
    {
        if (m_active)
            *m_stream << value;
        return *this;
    }

    // Qt 6 的 QDebug 不提供 std::string 重载（避免把 <string> 引入 qdebug.h），
    // 而 WebRTC 侧大量使用 std::string。缺此非模板重载则每个调用点都要手写
    // QString::fromStdString。非模板重载的优先级高于上面的模板。
    Line &operator<<(const std::string &value)
    {
        if (m_active)
            *m_stream << QString::fromStdString(value);
        return *this;
    }

private:
    bool m_active = false;
    Level m_level = Level::Info;
    const char *m_tag = "";
    QByteArray m_buffer;
    // 必须用指针持有：QDebug 要等自身析构时才把内容写进缓冲区，而成员变量在
    // Line 析构函数执行完毕后才销毁——若放成员，析构函数中读到的 m_buffer 恒为空。
    // 被过滤掉的行不构造它。一条日志即一个临时 Line，析构时落盘，整行一次写入，
    // 多线程下不会互相插入半行。
    std::unique_ptr<QDebug> m_stream;
};

} // namespace Log

#define LOG_TRACE(tag) ::Log::Line(::Log::Level::Trace, tag)
#define LOG_DEBUG(tag) ::Log::Line(::Log::Level::Debug, tag)
#define LOG_INFO(tag) ::Log::Line(::Log::Level::Info, tag)
#define LOG_WARN(tag) ::Log::Line(::Log::Level::Warn, tag)
#define LOG_ERROR(tag) ::Log::Line(::Log::Level::Error, tag)
#define LOG_FATAL(tag) ::Log::Line(::Log::Level::Fatal, tag)
