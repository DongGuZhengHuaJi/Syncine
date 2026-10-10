#include "Log.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <ostream>
#include <sstream>
#include <thread>
#include <unistd.h> // isatty

namespace {

// ── 文件策略,和客户端保持一致 ──────────────────────────────
constexpr long long kMaxFileBytes = 8LL * 1024 * 1024;
constexpr int kMaxRolls = 3;
constexpr int kKeepDays = 7;
constexpr int kTagWidth = 10;

struct State {
    std::string prefix = "server";
    std::string dir;
    std::string path;
    std::ofstream file;
    bool console = true;
};

State g_state;
std::mutex g_fileMutex; // 保护 g_state 里的文件与路径
std::atomic<int> g_level{static_cast<int>(Log::Level::Info)};

// 线程编号:std::thread::id → 1,2,3……,日志里显示成 [t2] 这种
std::mutex g_threadMutex;
std::map<std::thread::id, int> g_threadNumbers;
int g_nextThreadNumber = 1;
thread_local int t_threadNumber = -1;

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

// 只在输出到终端时用,重定向到文件时不要这些转义序列
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
    localtime_r(&seconds, &tm);

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
    localtime_r(&seconds, &tm);
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
        // 文件打不开也不能把服务带崩:退化成"只写控制台"
        std::fprintf(stderr, "[Log] 无法打开日志文件 %s,本次只写控制台\n",
                     g_state.path.c_str());
        return;
    }

    g_state.file << "\n──────── " << timestamp() << " 启动 ────────\n";
    g_state.file.flush();
}

void rotateIfNeededLocked(std::size_t incoming)
{
    if (!g_state.file.is_open())
        return;

    std::error_code sizeEc;
    const auto size = static_cast<long long>(
        std::filesystem::file_size(g_state.path, sizeEc));
    if (size + static_cast<long long>(incoming) <= kMaxFileBytes)
        return;

    g_state.file.close();

    std::error_code ec;
    std::filesystem::remove(rollPath(kMaxRolls), ec); // 最旧的直接扔
    for (int i = kMaxRolls - 1; i >= 1; --i)
        std::filesystem::rename(rollPath(i), rollPath(i + 1), ec);
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

} // namespace

// ══════════════════════════════════════════════════════════════

void Log::init(const std::string &filePrefix)
{
    g_state.prefix = filePrefix;

    const std::string envLevel = envOr("SYNCINE_LOG_LEVEL", "");
    if (!envLevel.empty())
        g_level.store(static_cast<int>(levelFromName(envLevel)));

    // 默认写到工作目录下的 logs/,SYNCINE_LOG_DIR 可以覆盖
    const std::string baseDir = envOr("SYNCINE_LOG_DIR", ".");
    g_state.dir = baseDir + "/logs";
    g_state.path = g_state.dir + "/" + filePrefix + "-" + today() + ".log";

    {
        std::lock_guard<std::mutex> lock(g_fileMutex);
        cleanupOldFilesLocked();
        openFileLocked();
    }

    LOG_INFO("Log") << "日志级别 " << levelName(level())
                    << " 文件: " << std::filesystem::absolute(g_state.path).string();
}

void Log::shutdown()
{
    std::lock_guard<std::mutex> lock(g_fileMutex);
    if (g_state.file.is_open()) {
        g_state.file.flush();
        g_state.file.close();
    }
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

    // 线程号先取(它自己会锁线程表),文件锁最后才拿 —— 固定这个顺序不会死锁
    const int number = threadNumber();
    const std::string stamp = timestamp();
    const std::string letter = levelLetter(level);

    std::string paddedTag = tag;
    if (paddedTag.size() < kTagWidth)
        paddedTag.append(kTagWidth - paddedTag.size(), ' ');

    std::ostringstream line;
    line << stamp << " [" << letter << "] [" << paddedTag << "] [t" << number
         << "] " << message;
    const std::string plain = line.str();

    std::lock_guard<std::mutex> lock(g_fileMutex);

    if (g_state.console) {
        // 警告以上走 stderr,其余走 stdout
        FILE *out = static_cast<int>(level) >= static_cast<int>(Level::Warn)
                        ? stderr : stdout;
        if (isatty(fileno(out))) {
            std::fprintf(out, "%s [%s%s\033[0m] [%s] [t%d] %s\n",
                         stamp.c_str(), levelColor(level), letter.c_str(),
                         paddedTag.c_str(), number, message.c_str());
        } else {
            std::fprintf(out, "%s\n", plain.c_str());
        }
        std::fflush(out);
    }

    if (g_state.file.is_open()) {
        rotateIfNeededLocked(plain.size() + 1);
        g_state.file << plain << '\n';
        // 每行都 flush:日志文件的意义就是"进程挂了也能看到最后一行"
        g_state.file.flush();
    }
}

// ── Line ───────────────────────────────────────────────────

Log::Line::Line(Level level, const char *tag)
    : m_active(isEnabled(level)), m_level(level), m_tag(tag)
{
}

Log::Line::~Line()
{
    if (!m_active)
        return;

    write(m_level, m_tag, m_os.str());
}
