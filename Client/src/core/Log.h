#pragma once

#include <QByteArray>
#include <QDebug>
#include <QString>

#include <memory>
#include <string>

// 全局日志:同时写控制台和本地文件。
//
// 用法:
//     LOG_INFO("WebRTC") << "已建立对端连接:" << peerId;
//     LOG_WARN("Playback") << "收到的音频格式不符合预期,已丢弃:" << format;
//
// 几条设计上的取舍,改这个文件之前先看一眼:
//
//  · **先判断级别再拼字符串**。Line 的构造函数里就把"这条要不要打"定下来,
//    被过滤掉的行连 operator<< 都不执行 —— 否则 60fps 路径上的
//    LOG_TRACE 会把 CPU 全花在拼字符串再丢掉。
//
//  · **一条日志 = 一个临时 Line 对象**,析构时才落盘。所以整行是一条完整
//    写入,多线程下不会互相插进半行(Eigen/WebRTC 的回调来自别的线程)。
//
//  · **不走 qDebug 输出**。syslog/文件写入是自己做的,只借 QDebug 当格式化器;
//    否则 init() 装了消息处理器之后会自己调自己,无限递归。
//
//  · init() 会装 Qt 消息处理器,qDebug/qWarning 以及 QML 里的 console.log
//    都会被收进同一个文件 —— QML 出问题时不用另开终端看输出。
namespace Log {

enum class Level : int { Trace = 0, Debug, Info, Warn, Error, Fatal };

// 打开日志文件、装 Qt 消息处理器。main() 里创建完 QGuiApplication 就该调,
// 越早越好:这样后面所有对象的构造日志都能落进文件。
void init(const QString &filePrefix = QStringLiteral("syncine"));
void shutdown();

void setLevel(Level level);
Level level();
bool isEnabled(Level level);

// 给当前线程起个名字。每个线程在日志里显示为 [t1]/[t2]……,
// 起过名字之后会多打一行说明,排查"回调到底在哪个线程"时用得上。
void nameCurrentThread(const char *name);

QString filePath();

// 直接写一行(一般不直接调,用上面的宏)
void write(Level level, const char *tag, const QString &message);

// 流式的一行。不要自己建对象,用 LOG_XXX 宏。
class Line {
public:
    Line(Level level, const char *tag);

    ~Line();

    template <typename T>
    Line &operator<<(const T &value)
    {
        if (m_active)
            *m_stream << value;
        return *this;
    }

    // Qt 6 的 QDebug 不带 std::string 重载(它不想把 <string> 拉进 qdebug.h),
    // 而 WebRTC 那边到处是 std::string —— 不补这一条,每个调用点都得手写
    // QString::fromStdString。非模板重载,优先级高于上面的模板。
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
    // 用指针而不是直接放成员:QDebug 攒够了内容也要等**它自己析构**才写进
    // 缓冲区,而成员是在 Line 的析构函数跑完之后才销毁的。放成员的话,
    // 析构函数里读 m_buffer 永远是空的(踩过)。被过滤掉的行根本不建它。
    std::unique_ptr<QDebug> m_stream;
};

} // namespace Log

#define LOG_TRACE(tag) ::Log::Line(::Log::Level::Trace, tag)
#define LOG_DEBUG(tag) ::Log::Line(::Log::Level::Debug, tag)
#define LOG_INFO(tag) ::Log::Line(::Log::Level::Info, tag)
#define LOG_WARN(tag) ::Log::Line(::Log::Level::Warn, tag)
#define LOG_ERROR(tag) ::Log::Line(::Log::Level::Error, tag)
#define LOG_FATAL(tag) ::Log::Line(::Log::Level::Fatal, tag)
