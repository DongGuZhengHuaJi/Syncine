//
// Cloudflare TURN 临时凭据。
//
// TURN 不能使用长期密钥：Cloudflare 要求用 TURN Token ID + API token 换取短时效的
// username/credential，而 API token 是账户级密钥，只能留在服务端（见 Secrets.h）。
// 故由服务端兑换，随 room_created / room_joined 下发给客户端。
//

#ifndef SERVER_TURNCREDENTIALS_H
#define SERVER_TURNCREDENTIALS_H

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

// 叶子依赖：不 include LogicSystem，后台线程也绝不回调它——否则会与
// LogicSystem::m_roomsMutex 形成锁序环（该路径上正持锁调用 iceServers()）。
class TurnCredentials {
public:
    static TurnCredentials &getInstance();

    // 起后台兑换线程（幂等）。必须晚于 Log::init，线程开工第一件事即打日志。
    // 一次兑换需数百毫秒（TLS 握手 + 一个来回），放入后台以免堵住 join_room 路径。
    void start();

    // 停线程（幂等，内部 join）。必须早于 Log::shutdown，否则收尾日志会丢。
    void stop();

    // 直接塞进协议消息的形状（与 Cloudflare 的 iceServers 数组一致）：
    //   [ {"urls":[...]},                                          ← STUN，无需鉴权
    //     {"urls":[...], "username":"...", "credential":"..."} ]     ← TURN，带临时凭据
    //
    // 任意线程可调，返回当前快照。永远返回非空数组：兑换失败时保留仍在有效期内的
    // 旧值，彻底无可用值才退回仅含 STUN 的兜底列表（至少还能直连）。
    nlohmann::json iceServers() const;

private:
    TurnCredentials() = default;
    ~TurnCredentials();

    TurnCredentials(const TurnCredentials &) = delete;
    TurnCredentials &operator=(const TurnCredentials &) = delete;

    // 从 Secrets.h / 环境变量读入的账户密钥
    struct ApiConfig {
        std::string keyId;
        std::string apiToken;

        bool complete() const {
            return !keyId.empty() && !apiToken.empty();
        }
    };

    // 密钥来源：环境变量优先，其次 Secrets.h。定义在 .cpp —— 只有那里知道
    // Secrets.h 是否存在（#if __has_include）。
    static ApiConfig loadApiConfig();

    // 后台线程主循环：兑换 → 睡到刷新时刻 → 再兑换。凭据有效期 24 小时。
    void run();

    // 兑换一次，成功则更新缓存并返回 true
    bool fetch(const ApiConfig &cfg);

    // ---- 缓存：后台线程写，任意线程读，由 m_mutex 保护 ----
    mutable std::mutex m_mutex;
    nlohmann::json m_iceServers;                          // null = 尚未成功过
    std::chrono::steady_clock::time_point m_validUntil{}; // 到期时间（兑换时刻 + ttl）

    // ---- 以下仅后台线程访问，无需加锁 ----
    std::chrono::steady_clock::time_point m_nextAttempt{};
    int m_failures = 0;

    // ---- 起停：跨线程，由 m_waitMutex 保护 ----
    std::thread m_thread;
    std::mutex m_waitMutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    bool m_started = false;
};

#endif //SERVER_TURNCREDENTIALS_H
