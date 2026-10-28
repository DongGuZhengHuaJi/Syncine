#include "Log.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <ostream>
#include <sstream>
#include <thread>
#ifdef _WIN32
#  include <io.h> // _isatty / _fileno
#  define isatty _isatty
#  define fileno _fileno
#else
#  include <unistd.h> // isatty
#endif

// localtime_r 是 POSIX 的；MSVC 的等价物叫 localtime_s，而且参数顺序相反。
namespace {
inline void syncineLocaltime(const std::time_t *seconds, std::tm *out)
{
#ifdef _WIN32
    localtime_s(out, seconds);
#else
    localtime_r(seconds, out);
#endif
}
} // namespace

namespace {

// ── 文件策略，与客户端保持一致 ──────────────────────────────
constexpr long long kMaxFileBytes = 8LL * 1024 * 1024;
constexpr int kMaxRolls = 3;
constexpr int kKeepDays = 7;
constexpr int kTagWidth = 10;

// ── 异步队列策略，与客户端保持一致 ──────────────────────────
constexpr long long kMaxQueueBytes = 4LL * 1024 * 1024;
constexpr long long kFlushBytes = 64LL * 1024;
constexpr int kFlushMs = 100;

struct State {
    std::string prefix = "server";
    std::string dir;
    std::string path;
    std::ofstream file;
    long long writtenBytes = 0;
    bool console = true;
    bool colorOut = false;
    bool colorErr = false;
};

struct Entry {
    std::string plain;
    std::string colored; // 仅在终端且需要颜色时非空
    Log::Level level = Log::Level::Info;
};

State g_state;
std::mutex g_fileMutex; // 保护文件、路径与写入

std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::deque<Entry> g_queue;
long long g_queueBytes = 0;
bool g_stopWriter = false;
bool g_writerRunning = false;
bool g_accepting = false;
std::thread g_writer;
std::atomic<bool> g_syncMode{false};
std::atomic<long long> g_droppedLow{0};

std::atomic<int> g_level{static_cast<int>(Log::Level::Info)};

// 线程编号：std::thread::id → 1,2,3……，日志中显示为 [t2]
std::mutex g_threadMutex;
std::map<std::thread::id, int> g_threadNumbers;
int g_nextThreadNumber = 1;
thread_local int t_threadNumber = -1;

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

// 仅用于输出到终端；重定向到文件时不应包含这些转义序列
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

Log::Level levelFromName(std::string name)
{
    for (char &c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (name == "trace") return Log::Level::Trace;
    if (name == "debug") return Log::Level::Debug;
    if (name == "warn" || name == "warning") return Log::Level::Warn;
    if (name == "error") return Log::Level::Error;
    if (name == "fatal") return Log::Level::Fatal;
    return Log::Level::Info;
}

std::string envOr(const char *key, const std::string &fallback)
{
    const char *value = std::getenv(key);
    return (value != nullptr && *value != '\0') ? std::string(value) : fallback;
}

bool envSet(const char *key)
{
    const char *value = std::getenv(key);
    return value != nullptr && *value != '\0' && std::string(value) != "0";
}

int threadNumber()
{
    if (t_threadNumber >= 0)
        return t_threadNumber;

    std::lock_guard<std::mutex> lock(g_threadMutex);
    const std::thread::id id = std::this_thread::get_id();
    const auto it = g_threadNumbers.find(id);
    t_threadNumber = (it != g_threadNumbers.end()) ? it->second : g_nextThreadNumber++;
    g_threadNumbers.insert({id, t_threadNumber});
    return t_threadNumber;
}

std::string timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count() % 1000;

    std::tm tm{};
    syncineLocaltime(&seconds, &tm);

    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm);

    std::ostringstream out;
    out << buffer << '.' << std::setfill('0') << std::setw(3) << millis;
    return out.str();
}

std::string today()
{
    const std::time_t seconds = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm{};
    syncineLocaltime(&seconds, &tm);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm);
    return buffer;
}

std::string rollPath(int index)
{
    return g_state.path + "." + std::to_string(index);
}

// 下面三个 open/rotate/cleanup 都要求已经持有 g_fileMutex
void openFileLocked()
{
    std::error_code ec;
    std::filesystem::create_directories(g_state.dir, ec);

    g_state.file.open(g_state.path, std::ios::out | std::ios::app);
    if (!g_state.file.is_open()) {
        // 文件打不开也不能把服务带崩：退化成"只写控制台"
        std::fprintf(stderr, "[Log] 无法打开日志文件 %s,本次只写控制台\n",
                     g_state.path.c_str());
        return;
    }

    g_state.file << "\n──────── " << timestamp() << " 启动 ────────\n";
    g_state.file.flush();
    g_state.writtenBytes = static_cast<long long>(
        std::filesystem::file_size(g_state.path, ec));
}

void rotateIfNeededLocked(long long incoming)
{
    if (!g_state.file.is_open())
        return;
    if (g_state.writtenBytes + incoming <= kMaxFileBytes)
        return;

    g_state.file.close();

    std::error_code ec;
    std::filesystem::remove(rollPath(kMaxRolls), ec); // 最旧的直接扔
    for (int i = kMaxRolls - 1; i >= 1; --i) {
        // Windows 上 rename 目标存在会失败：先删目标再改名。
        std::filesystem::remove(rollPath(i + 1), ec);
        std::filesystem::rename(rollPath(i), rollPath(i + 1), ec);
    }
    std::filesystem::remove(rollPath(1), ec);
    std::filesystem::rename(g_state.path, rollPath(1), ec);
    openFileLocked();
}

void cleanupOldFilesLocked()
{
    std::error_code ec;
    const std::filesystem::path dir(g_state.dir);
    if (!std::filesystem::exists(dir, ec))
        return;

    const auto deadline = std::filesystem::file_time_type::clock::now()
                          - std::chrono::hours(24 * kKeepDays);

    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind(g_state.prefix + "-", 0) != 0)
            continue;
        if (name.find(".log") == std::string::npos)
            continue;
        std::error_code timeEc;
        if (std::filesystem::last_write_time(entry, timeEc) < deadline)
            std::filesystem::remove(entry, ec);
    }
}

Entry makeEntry(Log::Level level, const char *tag, const std::string &message)
{
    // 行在调用线程定型：时间戳是事件发生的时间，tag 立刻拷贝。
    const int number = threadNumber();
    const std::string stamp = timestamp();
    const std::string letter = levelLetter(level);

    std::string paddedTag = tag;
    if (paddedTag.size() < kTagWidth)
        paddedTag.append(kTagWidth - paddedTag.size(), ' ');

    std::ostringstream line;
    line << stamp << " [" << letter << "] [" << paddedTag << "] [t" << number
         << "] " << message;

    const bool wantedColor = static_cast<int>(level) >= static_cast<int>(Log::Level::Warn)
                                 ? g_state.colorErr
                                 : g_state.colorOut;

    Entry entry;
    entry.level = level;
    entry.plain = line.str();
    if (wantedColor) {
        std::ostringstream colored;
        colored << stamp << " [" << levelColor(level) << letter << "\033[0m] ["
                << paddedTag << "] [t" << number << "] " << message;
        entry.colored = colored.str();
    }
    return entry;
}

// 调用方必须持有 g_fileMutex
void writeEntryLocked(const Entry &entry, bool flush)
{
    if (g_state.console) {
        FILE *out = static_cast<int>(entry.level) >= static_cast<int>(Log::Level::Warn)
                        ? stderr : stdout;
        const std::string &line = entry.colored.empty() ? entry.plain : entry.colored;
        std::fprintf(out, "%s\n", line.c_str());
        if (flush)
            std::fflush(out);
    }

    if (g_state.file.is_open()) {
        rotateIfNeededLocked(static_cast<long long>(entry.plain.size()) + 1);
        g_state.file << entry.plain << '\n';
        g_state.writtenBytes += static_cast<long long>(entry.plain.size()) + 1;
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
    if (g_state.file.is_open())
        g_state.file.flush();
}

void writeImmediate(const Entry &entry)
{
    std::lock_guard<std::mutex> lock(g_fileMutex);
    writeEntryLocked(entry, /*flush=*/true);
}

bool enqueueEntry(const Entry &entry)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (!g_accepting)
        return false;

    const long long size = static_cast<long long>(entry.plain.size());
    if (g_queueBytes + size > kMaxQueueBytes) {
        if (static_cast<int>(entry.level) < static_cast<int>(Log::Level::Warn)) {
            g_droppedLow.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    g_queue.push_back(entry);
    g_queueBytes += size;
    g_queueCv.notify_one();
    return true;
}

void writerLoop()
{
    constexpr auto kBatchWait = std::chrono::milliseconds(20);

    for (;;) {
        std::deque<Entry> batch;
        bool stopping = false;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            const auto start = std::chrono::steady_clock::now();
            while (!g_stopWriter && g_queueBytes < kFlushBytes) {
                if (std::chrono::steady_clock::now() - start
                    >= std::chrono::milliseconds(kFlushMs)) {
                    break;
                }
                g_queueCv.wait_for(lock, kBatchWait);
            }

            batch.swap(g_queue);
            g_queueBytes = 0;
            stopping = g_stopWriter;
        }

        {
            std::lock_guard<std::mutex> fileLock(g_fileMutex);
            for (const Entry &entry : batch)
                writeEntryLocked(entry, /*flush=*/false);
            flushLocked();
        }

        const long long dropped = g_droppedLow.exchange(0, std::memory_order_relaxed);
        if (dropped > 0) {
            Entry summary = makeEntry(
                Log::Level::Warn, "Log",
                "日志队列已满，丢弃 " + std::to_string(dropped) + " 条 trace/debug 日志");
            std::lock_guard<std::mutex> fileLock(g_fileMutex);
            writeEntryLocked(summary, /*flush=*/true);
        }

        if (stopping)
            break;
    }
}

} // namespace

// ══════════════════════════════════════════════════════════════

void Log::init(const std::string &filePrefix)
{
    if (g_initialized.load())
        return;
    g_initialized.store(true);

    g_state.prefix = filePrefix;

    const std::string envLevel = envOr("SYNCINE_LOG_LEVEL", "");
    if (!envLevel.empty())
        g_level.store(static_cast<int>(levelFromName(envLevel)));

    g_syncMode.store(envSet("SYNCINE_LOG_SYNC"));

    // 默认写到工作目录下的 logs/,SYNCINE_LOG_DIR 可以覆盖
    const std::string baseDir = envOr("SYNCINE_LOG_DIR", ".");
    g_state.dir = baseDir + "/logs";
    g_state.path = g_state.dir + "/" + filePrefix + "-" + today() + ".log";
    g_state.colorOut = isatty(fileno(stdout)) != 0;
    g_state.colorErr = isatty(fileno(stderr)) != 0;

    {
        std::lock_guard<std::mutex> lock(g_fileMutex);
        cleanupOldFilesLocked();
        openFileLocked();
    }

    if (!g_syncMode.load()) {
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            g_stopWriter = false;
            g_accepting = true;
            g_queue.clear();
            g_queueBytes = 0;
        }
        g_writer = std::thread(writerLoop);
        g_writerRunning = true;
    } else {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_accepting = true;
    }

    LOG_INFO("Log") << "日志级别: " << levelName(level())
                    << " 文件: " << std::filesystem::absolute(g_state.path).string()
                    << (g_syncMode.load() ? " (同步模式)" : " (异步模式)");
}

void Log::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_accepting = false;
    }

    if (g_writerRunning) {
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            g_stopWriter = true;
        }
        g_queueCv.notify_all();
        g_writer.join();
        g_writerRunning = false;
    }

    std::lock_guard<std::mutex> lock(g_fileMutex);
    if (g_state.file.is_open()) {
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
    LOG_INFO("Log") << "线程 t" << number << " → " << name;
}

std::string Log::filePath()
{
    std::lock_guard<std::mutex> lock(g_fileMutex);
    return g_state.path;
}

void Log::write(Level level, const char *tag, const std::string &message)
{
    if (!isEnabled(level))
        return;

    const Entry entry = makeEntry(level, tag, message);

    if (level == Level::Fatal || g_syncMode.load() || !g_initialized.load()) {
        writeImmediate(entry);
        return;
    }

    if (!enqueueEntry(entry))
        writeImmediate(entry);
}

// ── Line ───────────────────────────────────────────────────

Log::Line::Line(Level level, const char *tag)
    : m_active(isEnabled(level)), m_level(level), m_tag(tag)
{
    if (m_active)
        m_os = std::make_unique<std::ostringstream>();
}

Log::Line::~Line()
{
    if (!m_active || !m_os)
        return;
    write(m_level, m_tag, m_os->str());
}
