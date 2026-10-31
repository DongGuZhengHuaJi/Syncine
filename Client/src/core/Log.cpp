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
#include <QWaitCondition>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <io.h> // _isatty / _fileno
#  include <windows.h>
#  define isatty _isatty
#  define fileno _fileno
#else
#  include <unistd.h> // isatty
#endif

namespace {

// Windows 控制台默认代码页是 936（GBK），而日志统一以 UTF-8 字节写入
// stdout/stderr。不切代码页时，fprintf 出去的 UTF-8 中文会被控制台按 GBK
// 解释，出现「寮€濮嬪叡浜?」这类乱码。这里把当前控制台切到 UTF-8，并尽量
// 打开 VT 转义支持（日志里的颜色是 ANSI 序列）。
// 输出被重定向到文件/管道时，改代码页不影响原始字节，日志文件仍是 UTF-8。
void configureWindowsConsole()
{
#ifdef _WIN32
    if (GetConsoleWindow() == nullptr)
        return;

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    auto enableVirtualTerminal = [](DWORD stdHandle) {
        const HANDLE handle = GetStdHandle(stdHandle);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
            return;
        DWORD mode = 0;
        if (!GetConsoleMode(handle, &mode))
            return;
#ifdef ENABLE_VIRTUAL_TERMINAL_PROCESSING
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
#endif
        SetConsoleMode(handle, mode);
    };
    enableVirtualTerminal(STD_OUTPUT_HANDLE);
    enableVirtualTerminal(STD_ERROR_HANDLE);
#endif
}

// ── 文件策略，调整此处即可 ──────────────────────────────────
constexpr qint64 kMaxFileBytes = 8 * 1024 * 1024; // 单文件达到 8MB 即切分
constexpr int kMaxRolls = 3;                      // 同一天最多保留 .1 .2 .3
constexpr int kKeepDays = 7;                      // 启动时删除超过 7 天的旧日志
constexpr int kTagWidth = 10;                     // 标签对齐宽度

// ── 异步队列策略 ────────────────────────────────────────────
// 4MB 约等于几万条普通日志；写线程每 100ms 或 64KB 落盘一次。
constexpr qint64 kMaxQueueBytes = 4 * 1024 * 1024;
constexpr qint64 kFlushBytes = 64 * 1024;
constexpr int kFlushMs = 100;

struct State {
    QString prefix = QStringLiteral("syncine");
    QString dir;
    QString path;
    QFile file;
    qint64 writtenBytes = 0;
    bool console = true;
    bool colorOut = false;
    bool colorErr = false;
};

struct Entry {
    QByteArray plain;
    QByteArray colored; // 仅在终端且需要颜色时非空
    Log::Level level = Log::Level::Info;
};

State g_state;
QMutex g_fileMutex; // 保护 g_state 中的文件、路径与写入

// 队列：所有线程入队，单个写线程出队。队列锁与文件锁分开，
// 生产者不会被磁盘/终端 I/O 阻塞。
QMutex g_queueMutex;
QWaitCondition g_queueCv;
std::deque<Entry> g_queue;
qint64 g_queueBytes = 0;
bool g_stopWriter = false;
bool g_writerRunning = false;
bool g_accepting = false;
std::thread g_writer;
std::atomic<bool> g_syncMode{false};
std::atomic<qint64> g_droppedLow{0};

std::atomic<int> g_level{static_cast<int>(Log::Level::Info)};

// 线程编号：Qt::HANDLE → 1,2,3……，日志中显示为 [t2]，用于确认回调所在线程。
// 首次查询后缓存到 thread_local，后续无需加锁。
QMutex g_threadMutex;
QHash<Qt::HANDLE, int> g_threadNumbers;
int g_nextThreadNumber = 1;
thread_local int t_threadNumber = -1;

QtMessageHandler g_previousHandler = nullptr;
std::atomic<bool> g_initialized{false};

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

// 仅用于控制台。只在实际输出到终端时使用，重定向到文件时不应包含转义序列。
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

// 以下 open/rotate/cleanup 三个函数均要求调用方已持有 g_fileMutex

#ifdef _WIN32
// 旧版本写出的日志没有 BOM；Windows 上部分查看器会按 ANSI/GBK 打开。
// 启动时把当天已有的日志补成 UTF-8 BOM 版本，避免用户升级后仍看到旧文件乱码。
// 文件最大 8MB，一次性重写成本可以接受；已有 BOM 的文件不动。
void ensureLogFileHasBomLocked()
{
    static const char kBom[] = {'\xEF', '\xBB', '\xBF'};
    QFile probe(g_state.path);
    if (!probe.exists() || probe.size() == 0)
        return;
    if (!probe.open(QIODevice::ReadOnly))
        return;
    const QByteArray content = probe.readAll();
    probe.close();
    if (content.startsWith(QByteArray(kBom, 3)))
        return;

    QFile rewrite(g_state.path);
    if (!rewrite.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    rewrite.write(kBom, 3);
    rewrite.write(content);
    rewrite.close();
}
#endif

void openFileLocked()
{
    QDir().mkpath(g_state.dir);
#ifdef _WIN32
    ensureLogFileHasBomLocked();
#endif
    g_state.file.setFileName(g_state.path);

    if (!g_state.file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        // 文件打不开也不能把应用带崩：退化成"只写控制台"
        std::fprintf(stderr, "[Log] 无法打开日志文件 %s,本次只写控制台\n",
                     qPrintable(g_state.path));
        return;
    }

    // Windows 上很多查看器（记事本、部分日志工具）按 ANSI/GBK 打开无 BOM
    // 的文本，UTF-8 中文会显示成乱码。只给新建的空日志文件写 UTF-8 BOM，
    // 已有文件不追加（否则 BOM 会跑到文件中间）。Linux 不需要 BOM。
#ifdef _WIN32
    if (g_state.file.size() == 0)
        g_state.file.write("\xEF\xBB\xBF", 3);
#endif

    const QByteArray banner = QStringLiteral("\n──────── %1 启动 ────────\n")
                                  .arg(QDateTime::currentDateTime().toString(
                                      QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                                  .toUtf8();
    g_state.file.write(banner);
    g_state.file.flush();
    g_state.writtenBytes = g_state.file.size();
}

void rotateIfNeededLocked(qint64 incoming)
{
    if (!g_state.file.isOpen())
        return;
    if (g_state.writtenBytes + incoming <= kMaxFileBytes)
        return;

    g_state.file.close();
    QFile::remove(rollPath(kMaxRolls)); // 最旧的直接扔
    for (int i = kMaxRolls - 1; i >= 1; --i) {
        // Windows 上 rename 目标存在会失败；先删再改名。
        QFile::remove(rollPath(i + 1));
        QFile::rename(rollPath(i), rollPath(i + 1));
    }
    QFile::remove(rollPath(1));
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

Entry makeEntry(Log::Level level, const char *tag, const QString &message)
{
    // 行在这里就完整定型：时间戳是“事件发生的时间”，不是写线程落盘的时间。
    // tag 必须立刻拷贝——Qt message handler 传进来的 constData() 指向临时 QByteArray。
    const int number = threadNumber();
    const QString stamp = QDateTime::currentDateTime().toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    const QString letter = QString::fromLatin1(levelLetter(level));
    const QString paddedTag = QString::fromLatin1(tag).leftJustified(kTagWidth);
    const QString tail = QStringLiteral(" [%1] [t%2] %3")
                             .arg(paddedTag, QString::number(number), message);
    const QString plain = stamp + QStringLiteral(" [") + letter + QStringLiteral("]") + tail;

    const bool wantedColor = static_cast<int>(level) >= static_cast<int>(Log::Level::Warn)
                                 ? g_state.colorErr
                                 : g_state.colorOut;

    Entry entry;
    entry.level = level;
    entry.plain = plain.toUtf8();
    if (wantedColor) {
        entry.colored = (stamp + QStringLiteral(" [")
                         + QString::fromLatin1(levelColor(level)) + letter
                         + QStringLiteral("\033[0m]") + tail).toUtf8();
    }
    return entry;
}

// 写一条到控制台/文件；调用方必须持有 g_fileMutex。
// flush=true 时立即刷盘（Fatal、同步模式、批次结束）。
void writeEntryLocked(const Entry &entry, bool flush)
{
    if (g_state.console) {
        // 警告及以上走 stderr，其余走 stdout，使错误输出与普通输出分离
        FILE *out = static_cast<int>(entry.level) >= static_cast<int>(Log::Level::Warn)
                        ? stderr : stdout;
        const QByteArray &bytes = entry.colored.isEmpty() ? entry.plain : entry.colored;
        std::fprintf(out, "%s\n", bytes.constData());
        if (flush)
            std::fflush(out);
    }

    if (g_state.file.isOpen()) {
        rotateIfNeededLocked(entry.plain.size() + 1);
        g_state.file.write(entry.plain);
        g_state.file.write("\n", 1);
        g_state.writtenBytes += entry.plain.size() + 1;
        if (flush)
            g_state.file.flush();
    }
}

void flushLocked()
{
    if (g_state.console) {
        std::fflush(stdout);
        std::fflush(stderr);
    }
    if (g_state.file.isOpen())
        g_state.file.flush();
}

void writeImmediate(const Entry &entry)
{
    QMutexLocker locker(&g_fileMutex);
    writeEntryLocked(entry, /*flush=*/true);
}

bool enqueueEntry(const Entry &entry)
{
    QMutexLocker locker(&g_queueMutex);
    if (!g_accepting)
        return false;

    const qint64 size = entry.plain.size();
    if (g_queueBytes + size > kMaxQueueBytes) {
        // 队列满：低级别直接丢；Warn 及以上走慢路径，保证不被淹没。
        if (static_cast<int>(entry.level) < static_cast<int>(Log::Level::Warn)) {
            g_droppedLow.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    g_queue.push_back(entry);
    g_queueBytes += size;
    g_queueCv.wakeOne();
    return true;
}

void writerLoop()
{
    constexpr auto kBatchWait = std::chrono::milliseconds(20);

    for (;;) {
        std::deque<Entry> batch;
        bool stopping = false;
        {
            QMutexLocker locker(&g_queueMutex);
            const auto start = std::chrono::steady_clock::now();
            while (!g_stopWriter && g_queueBytes < kFlushBytes) {
                if (std::chrono::steady_clock::now() - start
                    >= std::chrono::milliseconds(kFlushMs)) {
                    break;
                }
                g_queueCv.wait(&g_queueMutex, kBatchWait.count());
            }

            batch.swap(g_queue);
            g_queueBytes = 0;
            stopping = g_stopWriter;
        }

        if (!batch.empty()) {
            QMutexLocker fileLocker(&g_fileMutex);
            for (const Entry &entry : batch)
                writeEntryLocked(entry, /*flush=*/false);
            flushLocked();
        } else {
            // 即使没有日志也定期 flush，保证控制台不长时间不刷新
            QMutexLocker fileLocker(&g_fileMutex);
            flushLocked();
        }

        const qint64 dropped = g_droppedLow.exchange(0, std::memory_order_relaxed);
        if (dropped > 0) {
            Entry summary;
            summary.level = Log::Level::Warn;
            const QString message = QStringLiteral("日志队列已满，丢弃 %1 条 trace/debug 日志")
                                        .arg(dropped);
            summary.plain = makeEntry(Log::Level::Warn, "Log", message).plain;
            QMutexLocker fileLocker(&g_fileMutex);
            writeEntryLocked(summary, /*flush=*/true);
        }

        if (stopping)
            break;
    }
}

// 将 Qt 自身的消息（qDebug/qWarning、QML 的 console.log）收进同一文件
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

    // 以分类名作为标签：QML 的 console.log 为 "qml"，裸 qDebug 为 "default"
    QByteArray tag = context.category ? QByteArray(context.category) : QByteArray("Qt");
    if (tag == "default")
        tag = "Qt";

    QString text = message;
    if (context.file) {
        // 只保留文件名：QML 给出的是 qrc:/qt/qml/SyncineApp/ui/….qml 这样的长路径
        const QString file = QString::fromUtf8(context.file);
        text += QStringLiteral(" (%1:%2)")
                    .arg(file.mid(file.lastIndexOf(QLatin1Char('/')) + 1))
                    .arg(context.line);
    }

    Log::write(level, tag.constData(), text);

    // Qt 默认处理器在 fatal 之后会 abort，需保留该行为
    if (type == QtFatalMsg)
        abort();
}

} // namespace

// ══════════════════════════════════════════════════════════════

void Log::init(const QString &filePrefix)
{
    if (g_initialized.load())
        return;
    g_initialized.store(true);

    // 必须在第一条日志输出之前执行，否则控制台仍按旧代码页解释 UTF-8。
    configureWindowsConsole();

    const QByteArray envLevel = qgetenv("SYNCINE_LOG_LEVEL");
    if (!envLevel.isEmpty())
        g_level.store(static_cast<int>(levelFromName(QString::fromLatin1(envLevel))));

    g_syncMode.store(qEnvironmentVariableIsSet("SYNCINE_LOG_SYNC"));

    g_state.prefix = filePrefix;

    // 默认跟随系统路径：~/.local/share/Syncine/logs/。
    // SYNCINE_LOG_DIR 可覆盖（例如指向工程目录，便于运行中 tail）
    const QByteArray envDir = qgetenv("SYNCINE_LOG_DIR");
    const QString baseDir = envDir.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : QString::fromLocal8Bit(envDir);

    g_state.dir = baseDir + QStringLiteral("/logs");
    g_state.path = QStringLiteral("%1/%2-%3.log")
                       .arg(g_state.dir, filePrefix,
                            QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
    g_state.colorOut = isatty(fileno(stdout)) != 0;
    g_state.colorErr = isatty(fileno(stderr)) != 0;

    {
        QMutexLocker locker(&g_fileMutex);
        cleanupOldFilesLocked();
        openFileLocked();
    }

    g_previousHandler = qInstallMessageHandler(qtMessageHandler);

    if (!g_syncMode.load()) {
        {
            QMutexLocker locker(&g_queueMutex);
            g_stopWriter = false;
            g_accepting = true;
            g_queue.clear();
            g_queueBytes = 0;
        }
        g_writer = std::thread(writerLoop);
        g_writerRunning = true;
    } else {
        QMutexLocker locker(&g_queueMutex);
        g_accepting = true;
    }

    // 注意 QDebug 会在每个 << 之间插入一个空格，故此处字符串不带尾随空格
    LOG_INFO("Log") << "日志级别:" << levelName(level()) << "文件:" << g_state.path
                    << (g_syncMode.load() ? "(同步模式)" : "(异步模式)");
}

void Log::shutdown()
{
    // 先停止接收新日志，再恢复 Qt handler；写线程此时可能还在排空队列。
    {
        QMutexLocker locker(&g_queueMutex);
        g_accepting = false;
    }

    if (g_previousHandler) {
        qInstallMessageHandler(g_previousHandler);
        g_previousHandler = nullptr;
    }

    if (g_writerRunning) {
        {
            QMutexLocker locker(&g_queueMutex);
            g_stopWriter = true;
            g_queueCv.wakeAll();
        }
        g_writer.join();
        g_writerRunning = false;
    }

    QMutexLocker locker(&g_fileMutex);
    if (g_state.file.isOpen()) {
        g_state.file.flush();
        g_state.file.close();
    }
    g_initialized.store(false);
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

    const Entry entry = makeEntry(level, tag, message);

    // Fatal 必须同步落盘，否则随后的 abort 会把队列里的最后一行带走。
    if (level == Level::Fatal || g_syncMode.load() || !g_initialized.load()) {
        writeImmediate(entry);
        return;
    }

    if (!enqueueEntry(entry)) {
        // 队列满且是 Warn 及以上，或 shutdown 已经开始：走同步慢路径。
        writeImmediate(entry);
    }
}

// ── Line ───────────────────────────────────────────────────

Log::Line::Line(Level level, const char *tag)
    : m_active(isEnabled(level)), m_level(level), m_tag(tag)
{
    if (m_active) {
        m_stream = std::make_unique<QDebug>(&m_buffer);
        m_stream->noquote(); // QDebug 默认给 QString 加双引号，日志中不需要
    }
}

Log::Line::~Line()
{
    if (!m_active)
        return;

    // 先释放它——QDebug 到这一步才把内容写入 m_buffer
    m_stream.reset();

    QString text = QString::fromUtf8(m_buffer);
    // QDebug 每写一个值都会追加一个空格（含最后一个），需去掉行尾空格
    while (text.endsWith(QLatin1Char('\n')) || text.endsWith(QLatin1Char(' ')))
        text.chop(1);

    write(m_level, m_tag, text);
}
