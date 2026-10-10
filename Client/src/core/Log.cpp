#include "core/Log.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <unistd.h> // isatty

namespace {

// ── 文件策略,改这里就够了 ──────────────────────────────────
constexpr qint64 kMaxFileBytes = 8 * 1024 * 1024; // 单个文件到 8MB 就切
constexpr int kMaxRolls = 3;                      // 同一天最多留 .1 .2 .3
constexpr int kKeepDays = 7;                      // 超过 7 天的旧日志启动时删掉
constexpr int kTagWidth = 10;                     // 标签对齐宽度

struct State {
    QString prefix = QStringLiteral("syncine");
    QString dir;
    QString path;
    QFile file;
    bool console = true;
};

State g_state;
QMutex g_fileMutex; // 保护 g_state 里的文件与路径
std::atomic<int> g_level{static_cast<int>(Log::Level::Info)};

// 线程编号:Qt::HANDLE → 1,2,3……。日志里显示成 [t2] 这种,
// 便于判断"这条回调到底在哪个线程"。查一次缓存到 thread_local,
// 之后连锁都不用加。
QMutex g_threadMutex;
QHash<Qt::HANDLE, int> g_threadNumbers;
int g_nextThreadNumber = 1;
thread_local int t_threadNumber = -1;

QtMessageHandler g_previousHandler = nullptr;
bool g_initialized = false;

const char *levelLetter(Log::Level level)
{
    switch (level) {
    case Log::Level::Trace: return "T";
    case Log::Level::Debug: return "D";
    case Log::Level::Info:  return "I";
    case Log::Level::Warn:  return "W";
    case Log::Level::Error: return "E";
    case Log::Level::Fatal: return "F";
    }
    return "?";
}

const char *levelName(Log::Level level)
{
    switch (level) {
    case Log::Level::Trace: return "trace";
    case Log::Level::Debug: return "debug";
    case Log::Level::Info:  return "info";
    case Log::Level::Warn:  return "warn";
    case Log::Level::Error: return "error";
    case Log::Level::Fatal: return "fatal";
    }
    return "?";
}

// 控制台专用。只在输出到终端时用,重定向到文件时不要这些转义序列
const char *levelColor(Log::Level level)
{
    switch (level) {
    case Log::Level::Trace: return "\033[90m";
    case Log::Level::Debug: return "\033[36m";
    case Log::Level::Info:  return "\033[32m";
    case Log::Level::Warn:  return "\033[33m";
    case Log::Level::Error: return "\033[31m";
    case Log::Level::Fatal: return "\033[1;31m";
    }
    return "";
}

Log::Level levelFromName(const QString &name)
{
    const QString lowered = name.trimmed().toLower();
    if (lowered == QLatin1String("trace")) return Log::Level::Trace;
    if (lowered == QLatin1String("debug")) return Log::Level::Debug;
    if (lowered == QLatin1String("warn") || lowered == QLatin1String("warning"))
        return Log::Level::Warn;
    if (lowered == QLatin1String("error")) return Log::Level::Error;
    if (lowered == QLatin1String("fatal")) return Log::Level::Fatal;
    return Log::Level::Info;
}

int threadNumber()
{
    if (t_threadNumber >= 0)
        return t_threadNumber;

    QMutexLocker locker(&g_threadMutex);
    const Qt::HANDLE id = QThread::currentThreadId();
    const auto it = g_threadNumbers.constFind(id);
    t_threadNumber = (it != g_threadNumbers.constEnd()) ? it.value() : g_nextThreadNumber++;
    g_threadNumbers.insert(id, t_threadNumber);
    return t_threadNumber;
}

QString rollPath(int index)
{
    return QStringLiteral("%1.%2").arg(g_state.path).arg(index);
}

// 下面三个 open/rotate/cleanup 都要求已经持有 g_fileMutex
void openFileLocked()
{
    QDir().mkpath(g_state.dir);
    g_state.file.setFileName(g_state.path);

    if (!g_state.file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        // 文件打不开也不能把应用带崩:退化成"只写控制台"
        std::fprintf(stderr, "[Log] 无法打开日志文件 %s,本次只写控制台\n",
                     qPrintable(g_state.path));
        return;
    }

    const QString banner = QStringLiteral("\n──────── %1 启动 ────────\n")
                               .arg(QDateTime::currentDateTime().toString(
                                   QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    g_state.file.write(banner.toUtf8());
    g_state.file.flush();
}

void rotateIfNeededLocked(qint64 incoming)
{
    if (!g_state.file.isOpen())
        return;
    if (g_state.file.size() + incoming <= kMaxFileBytes)
        return;

    g_state.file.close();
    QFile::remove(rollPath(kMaxRolls)); // 最旧的直接扔
    for (int i = kMaxRolls - 1; i >= 1; --i)
        QFile::rename(rollPath(i), rollPath(i + 1));
    QFile::rename(g_state.path, rollPath(1));
    openFileLocked();
}

void cleanupOldFilesLocked()
{
    QDir dir(g_state.dir);
    if (!dir.exists())
        return;

    const QDateTime deadline = QDateTime::currentDateTime().addDays(-kKeepDays);
    const QStringList filters{QStringLiteral("%1-*.log*").arg(g_state.prefix)};
    const QFileInfoList files = dir.entryInfoList(filters, QDir::Files);
    for (const QFileInfo &info : files) {
        if (info.lastModified() < deadline)
            QFile::remove(info.absoluteFilePath());
    }
}

// 把 Qt 自己的消息(qDebug/qWarning、QML 的 console.log)也收编进同一个文件
void qtMessageHandler(QtMsgType type, const QMessageLogContext &context,
                      const QString &message)
{
    Log::Level level = Log::Level::Debug;
    switch (type) {
    case QtDebugMsg:    level = Log::Level::Debug; break;
    case QtInfoMsg:     level = Log::Level::Info; break;
    case QtWarningMsg:  level = Log::Level::Warn; break;
    case QtCriticalMsg: level = Log::Level::Error; break;
    case QtFatalMsg:    level = Log::Level::Fatal; break;
    }

    // 分类名当标签:QML 的 console.log 是 "qml",裸 qDebug 是 "default"
    QByteArray tag = context.category ? QByteArray(context.category) : QByteArray("Qt");
    if (tag == "default")
        tag = "Qt";

    QString text = message;
    if (context.file) {
        // 只留文件名 —— QML 给的是 qrc:/qt/qml/SyncineApp/ui/....qml 一长串
        const QString file = QString::fromUtf8(context.file);
        text += QStringLiteral(" (%1:%2)")
                    .arg(file.mid(file.lastIndexOf(QLatin1Char('/')) + 1))
                    .arg(context.line);
    }

    Log::write(level, tag.constData(), text);

    // Qt 默认处理器在 fatal 之后会 abort,别把这个行为弄丢
    if (type == QtFatalMsg)
        abort();
}

} // namespace

// ══════════════════════════════════════════════════════════════

void Log::init(const QString &filePrefix)
{
    if (g_initialized)
        return;
    g_initialized = true;

    const QByteArray envLevel = qgetenv("SYNCINE_LOG_LEVEL");
    if (!envLevel.isEmpty())
        g_level.store(static_cast<int>(levelFromName(QString::fromLatin1(envLevel))));

    g_state.prefix = filePrefix;

    // 默认跟系统走:~/.local/share/Syncine/logs/。
    // SYNCINE_LOG_DIR 可以覆盖(比如指到工程目录里,方便边跑边 tail)
    const QByteArray envDir = qgetenv("SYNCINE_LOG_DIR");
    const QString baseDir = envDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : QString::fromLocal8Bit(envDir);

    g_state.dir = baseDir + QStringLiteral("/logs");
    g_state.path = QStringLiteral("%1/%2-%3.log")
                       .arg(g_state.dir, filePrefix,
                            QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));

    {
        QMutexLocker locker(&g_fileMutex);
        cleanupOldFilesLocked();
        openFileLocked();
    }

    g_previousHandler = qInstallMessageHandler(qtMessageHandler);

    LOG_INFO("Log") << "日志级别" << levelName(level()) << "文件:" << g_state.path;
}

void Log::shutdown()
{
    QMutexLocker locker(&g_fileMutex);

    if (g_state.file.isOpen()) {
        g_state.file.flush();
        g_state.file.close();
    }
    if (g_previousHandler) {
        qInstallMessageHandler(g_previousHandler);
        g_previousHandler = nullptr;
    }
    g_initialized = false;
}

void Log::setLevel(Level level)
{
    g_level.store(static_cast<int>(level));
}

Log::Level Log::level()
{
    return static_cast<Level>(g_level.load());
}

bool Log::isEnabled(Level level)
{
    return static_cast<int>(level) >= g_level.load();
}

void Log::nameCurrentThread(const char *name)
{
    const int number = threadNumber();
    LOG_INFO("Log") << "线程 t" << number << "→" << name;
}

QString Log::filePath()
{
    QMutexLocker locker(&g_fileMutex);
    return g_state.path;
}

void Log::write(Level level, const char *tag, const QString &message)
{
    if (!isEnabled(level))
        return;

    // 线程号先取(它自己会锁线程表),文件锁最后才拿。
    // 固定这个先后顺序:nameCurrentThread 里也是先线程表再写日志,不会死锁。
    const int number = threadNumber();
    const QString stamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString letter = QString::fromLatin1(levelLetter(level));
    const QString tail = QStringLiteral(" [%1] [t%2] %3")
                             .arg(QString::fromLatin1(tag).leftJustified(kTagWidth),
                                  QString::number(number), message);
    const QString plain = stamp + QStringLiteral(" [") + letter + QStringLiteral("]") + tail;

    QMutexLocker locker(&g_fileMutex);

    if (g_state.console) {
        // 警告以上走 stderr,其余走 stdout —— 保留原来"错误和普通输出分开"的习惯
        FILE *out = static_cast<int>(level) >= static_cast<int>(Level::Warn) ? stderr : stdout;
        if (isatty(fileno(out))) {
            const QString colored = stamp + QStringLiteral(" [")
                + QString::fromLatin1(levelColor(level)) + letter
                + QStringLiteral("\033[0m]") + tail;
            std::fprintf(out, "%s\n", qPrintable(colored));
        } else {
            std::fprintf(out, "%s\n", qPrintable(plain));
        }
        std::fflush(out);
    }

    if (g_state.file.isOpen()) {
        const QByteArray bytes = plain.toUtf8() + '\n';
        rotateIfNeededLocked(bytes.size());
        g_state.file.write(bytes);
        // 每行都 flush:日志文件的意义就是"程序崩了也能看到最后一行"
        g_state.file.flush();
    }
}

// ── Line ───────────────────────────────────────────────────

Log::Line::Line(Level level, const char *tag)
    : m_active(isEnabled(level)), m_level(level), m_tag(tag)
{
    if (m_active) {
        m_stream = std::make_unique<QDebug>(&m_buffer);
        m_stream->noquote(); // QDebug 默认给 QString 加双引号,日志里不要
    }
}

Log::Line::~Line()
{
    if (!m_active)
        return;

    // 先让它析构 —— QDebug 到这一步才把攒的内容写进 m_buffer
    m_stream.reset();

    QString text = QString::fromUtf8(m_buffer);
    // QDebug 每写一个值都会跟一个空格(包括最后一个),行尾的空格要去掉
    while (text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char(' ')))
        text.chop(1);

    write(m_level, m_tag, text);
}
