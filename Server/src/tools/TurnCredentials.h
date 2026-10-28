//
// TURN 临时凭据。
//
// 两种来源，按优先级：
//   1. 自建 coturn：SYNCINE_COTURN_URLS + SYNCINE_COTURN_SECRET。
//      coturn 用 use-auth-secret / static-auth-secret，服务端本地用
//      HMAC-SHA1 生成短时效 username/credential，不需要外部网络请求。
//   2. Cloudflare：未配置 coturn 时回退到原来的 TURN Token ID + API token
//      兑换流程（见 Secrets.h / SYNCINE_TURN_KEY_ID / SYNCINE_TURN_API_TOKEN）。
//
// 只要 coturn 配置完整，就不再启动 Cloudflare 兑换线程；Cloudflare 配置保留，
// 删掉 coturn 环境变量即可恢复使用。
//

#ifndef SERVER_TURNCREDENTIALS_H
#define SERVER_TURNCREDENTIALS_H

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

// 叶子依赖：不 include LogicSystem，后台线程也绝不回调它——否则会与
// LogicSystem::m_roomsMutex 形成锁序环（该路径上正持锁调用 iceServers()）。
class TurnCredentials {
public:
    static TurnCredentials &getInstance();

    // 起凭据服务（幂等）。必须晚于 Log::init。
    // coturn 配置完整时不启动后台线程；否则启动 Cloudflare 兑换线程。
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

    struct CoturnConfig {
        std::vector<std::string> turnUrls;
        std::vector<std::string> stunUrls;
        std::string secret;
        long ttlSeconds = 86400;
    };

    // 从 Secrets.h / 环境变量读入的 Cloudflare 账户密钥
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

    // coturn 配置：缺 URL 表示未配置，返回 nullopt；配置了 URL 但缺 secret
    // 视为配置错误，记录日志后回退 Cloudflare。
    static std::optional<CoturnConfig> loadCoturnConfig();

    // coturn REST API 的 credential = base64(HMAC-SHA1(secret, username))
    static std::string coturnCredential(const std::string &secret,
                                        const std::string &username);

    // 用 coturn 配置生成 iceServers JSON；生成/校验失败返回 null。
    static nlohmann::json buildCoturnIceServers(const CoturnConfig &config);

    // 后台线程主循环：兑换 → 睡到刷新时刻 → 再兑换。凭据有效期 24 小时。
    void run();

    // 兑换一次，成功则更新缓存并返回 true
    bool fetch(const ApiConfig &cfg);

    // ---- 缓存：后台线程写，任意线程读，由 m_mutex 保护 ----
    mutable std::mutex m_mutex;
    nlohmann::json m_iceServers;                          // null = 尚未成功过
    std::optional<CoturnConfig> m_coturn;                 // 有值 = 使用自建 coturn
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
