//
// Cloudflare TURN 的临时凭据。
//
// 背景:客户端建 PeerConnection 需要一份 ICE 服务器列表(STUN + TURN)。
// 其中 TURN 不能用长期密钥 —— Cloudflare 要求拿 TURN Token ID + API token 去
// 换一组**短时效**的 username/credential。而 API token 是账户级密钥,只能留在
// 服务端(见 Secrets.h 顶部)。所以由服务端兑换,随 room_created / room_joined
// 一起下发给客户端。
//
// 为什么要有后台线程和缓存:
//   * 兑换一次要几百毫秒(TLS 握手 + 一个来回),不能让它堵在 join_room 的
//     路径上 —— 那是 LogicSystem 的工作线程,堵住的就是所有人的消息;
//   * 凭据有 24 小时的有效期,提前换比过期后再换好得多。
//
// 线程模型:
//   * start() 起一条后台线程,立刻兑换一次,之后按 ttl 提前刷新;stop() 会 join;
//   * iceServers() 任意线程可调,返回当前快照(拷贝),**永远不会返回空数组**;
//   * 兑换失败时**保留仍在有效期内的旧值**;彻底没有可用的(从未成功过,或者
//     已经硬过期)才退回只有 STUN 的兜底列表 —— 至少还能直连。
//
// 这个类是**叶子依赖**:不 include LogicSystem,后台线程也绝不回调它。
// 反过来做会和 LogicSystem::m_roomsMutex 形成锁序环(那条路径上正是持有
// m_roomsMutex 调 iceServers() 的)。
//

#ifndef SERVER_TURNCREDENTIALS_H
#define SERVER_TURNCREDENTIALS_H

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

class TurnCredentials {
public:
    static TurnCredentials &getInstance();

    // 起后台兑换线程(幂等)。必须在 Log::init 之后调 —— 它开工第一件事就是打日志。
    void start();

    // 停线程(幂等,内部 join)。要在 Log::shutdown 之前调,否则收尾日志会丢。
    void stop();

    // 直接塞进协议消息的形状(和 Cloudflare 的 iceServers 数组一致):
    //   [ {"urls":[...]},                                  ← STUN,不需要鉴权
    //     {"urls":[...], "username":"...", "credential":"..."} ]   ← TURN,带临时凭据
    //
    // 任意线程可调。永远返回非空数组,最差是只有 STUN 的兜底。
    nlohmann::json iceServers() const;

private:
    TurnCredentials() = default;
    ~TurnCredentials();

    TurnCredentials(const TurnCredentials &) = delete;
    TurnCredentials &operator=(const TurnCredentials &) = delete;

    // 从 Secrets.h / 环境变量读来的账户密钥
    struct ApiConfig {
        std::string keyId;
        std::string apiToken;

        bool complete() const {
            return !keyId.empty() && !apiToken.empty();
        }
    };

    // 账户密钥从哪来:环境变量优先,其次 Secrets.h。定义在 .cpp 里 ——
    // 只有那里知道 Secrets.h 到底存不存在(#if __has_include)。
    static ApiConfig loadApiConfig();

    // 后台线程的主循环:兑换 → 睡到该刷新的时候 → 再兑换
    void run();

    // 兑换一次。成功则更新缓存并返回 true。
    bool fetch(const ApiConfig &cfg);

    // ---- 缓存:下载线程写,任意线程读,由 m_mutex 保护 ----
    mutable std::mutex m_mutex;
    nlohmann::json m_iceServers;                          // null = 还从来没成功过
    std::chrono::steady_clock::time_point m_validUntil{}; // 到期时间(兑换时刻 + ttl)

    // ---- 下面这些只有后台线程碰,不需要锁 ----
    std::chrono::steady_clock::time_point m_nextAttempt{};
    int m_failures = 0;

    // ---- 起停:跨线程,由 m_waitMutex 保护 ----
    std::thread m_thread;
    std::mutex m_waitMutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    bool m_started = false;
};

#endif //SERVER_TURNCREDENTIALS_H
