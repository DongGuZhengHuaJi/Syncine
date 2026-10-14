//
// Created by donggu on 2026/10/14.
//
// 见 TurnCredentials.h 顶部的说明(为什么要有这个类、线程模型是怎样的)。
//

#include "TurnCredentials.h"

#include <algorithm>
#include <cstdlib>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h> // SSL_set_tlsext_host_name

#include "Log.h"

// Secrets.h 不进仓库(见 .gitignore)。没有它的机器走环境变量 ——
// 两条路都走不通也能跑,只是没有 TURN 中继(下面的 run() 会说清楚)。
#if __has_include("Secrets.h")
#  include "Secrets.h"
#endif

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;

using json = nlohmann::json;

namespace {

// Cloudflare 允许的最长 ttl 就是 24 小时,再长申请不到。
constexpr long kCredentialTtlSeconds = 86400;

// 用到 ttl 的多少就提前换。留出 1/4(6 小时)当重试窗口:
// 退避最长 15 分钟,这段时间够试二十几次。
constexpr double kRefreshAtRatio = 0.75;

constexpr int kRetryBaseSeconds = 30;
constexpr int kRetryMaxSeconds = 900; // 15 分钟
constexpr int kRequestTimeoutSeconds = 15;
constexpr const char *kApiHost = "rtc.live.cloudflare.com";
constexpr const char *kApiPort = "443";

// 没有可用凭据时的兜底:一个公共 STUN,局域网和普通 NAT 照样直连。
//
// **不能**写成 "stun:stun.cloudflare.com:443?transport=tcp":libwebrtc 的
// ICE URL 解析明确拒绝带 query 的 stun URL,而且那是个硬错误 ——
// 任何一个 URL 不合法,**整条 PeerConnection 都建不起来**(不是跳过这一项),
// 症状是客户端只报一句"连接创建失败,已放弃",极难排查。
json fallbackIceServers() {
    json stun;
    stun["urls"] = json::array({"stun:stun.cloudflare.com:3478"});
    return json::array({stun});
}

// 下发前先校验形状。理由同上:坏配置会让客户端**整条连接**失败,
// 不如在这里拦住、退回 STUN 兜底 —— 少一个中继,但连接还能建起来。
bool validateIceServers(const json &list) {
    if (!list.is_array() || list.empty()) {
        LOG_ERROR("Turn") << "拒绝下发的 ICE 配置:不是非空数组";
        return false;
    }

    for (const json &entry : list) {
        if (!entry.is_object() || !entry.contains("urls") || !entry["urls"].is_array()
            || entry["urls"].empty()) {
            LOG_ERROR("Turn") << "拒绝下发的 ICE 配置:有一项没有 urls";
            return false;
        }

        bool hasTurn = false;
        for (const json &url : entry["urls"]) {
            if (!url.is_string() || url.get<std::string>().empty()) {
                LOG_ERROR("Turn") << "拒绝下发的 ICE 配置:url 不是非空字符串";
                return false;
            }

            const std::string text = url.get<std::string>();
            const bool isStun = text.rfind("stun", 0) == 0;

            // stun URL 带 query 会被 libwebrtc 判为非法(见上面 fallbackIceServers 的说明)
            if (isStun && text.find('?') != std::string::npos) {
                LOG_ERROR("Turn") << "拒绝下发的 ICE 配置:STUN url 带了 query(会让整条"
                                     "连接建不起来):" << text;
                return false;
            }
            if (!isStun && text.rfind("turn", 0) == 0)
                hasTurn = true;
        }

        // TURN 项必须有凭据:username/credential 为空同样是硬错误
        const bool hasUsername = entry.contains("username") && entry["username"].is_string()
                                 && !entry["username"].get<std::string>().empty();
        const bool hasCredential = entry.contains("credential")
                                   && entry["credential"].is_string()
                                   && !entry["credential"].get<std::string>().empty();
        if (hasTurn && (!hasUsername || !hasCredential)) {
            LOG_ERROR("Turn") << "拒绝下发的 ICE 配置:TURN 项没有 username/credential";
            return false;
        }
    }

    return true;
}

} // namespace

// ============================
// 单例
// ============================

TurnCredentials &TurnCredentials::getInstance() {
    // C++11 起函数内 static 的初始化是线程安全的。注意别在静态初始化阶段碰它。
    static TurnCredentials instance;
    return instance;
}

TurnCredentials::~TurnCredentials() {
    stop(); // 兜底。正常路径是 main 里显式 stop —— 那时 Log 还开着
}

// ============================
// 账户密钥
// ============================

// 环境变量优先,其次 Secrets.h。环境变量优先是有意的 ——
// 想拿另一套 key 试一下时,不用改文件、不用重编。
TurnCredentials::ApiConfig TurnCredentials::loadApiConfig() {
    ApiConfig cfg;
    bool fromEnv = false;

#if __has_include("Secrets.h")
    cfg.keyId = Secrets::TURN_KEY_ID;
    cfg.apiToken = Secrets::TURN_API_TOKEN;
#endif

    if (const char *id = std::getenv("SYNCINE_TURN_KEY_ID")) {
        cfg.keyId = id;
        fromEnv = true;
    }
    if (const char *token = std::getenv("SYNCINE_TURN_API_TOKEN")) {
        cfg.apiToken = token;
        fromEnv = true;
    }

    if (cfg.complete())
        LOG_INFO("Turn") << "TURN 账户密钥来自 " << (fromEnv ? "环境变量" : "Secrets.h");

    return cfg;
}

// ============================
// 起停
// ============================

void TurnCredentials::start() {
    std::lock_guard<std::mutex> lock(m_waitMutex);
    if (m_started)
        return;

    m_started = true;
    m_stop = false;
    m_thread = std::thread([this] { run(); });
    LOG_INFO("Turn") << "TURN 凭据服务已启动";
}

void TurnCredentials::stop() {
    {
        std::lock_guard<std::mutex> lock(m_waitMutex);
        if (!m_started)
            return; // 幂等:再调一次直接走人(析构里还会再调一次)
        m_started = false;
        m_stop = true;
    }

    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join(); // 最坏等一次请求跑完(kRequestTimeoutSeconds 封顶)

    LOG_INFO("Turn") << "TURN 凭据服务已停止";
}

// ============================
// 后台线程
// ============================

void TurnCredentials::run() {
    Log::nameCurrentThread("turn");

    const ApiConfig cfg = loadApiConfig();
    if (!cfg.complete()) {
        // 不是致命错误:STUN 不需要鉴权,没有 TURN 也只是对称 NAT 后的对端
        // 连不上。所以打一行 WARN 说清楚,不要每次进房都刷一遍 ERROR。
        LOG_WARN("Turn") << "没有配置 TURN 密钥(Secrets.h 或 SYNCINE_TURN_KEY_ID / "
                            "SYNCINE_TURN_API_TOKEN),只会下发 STUN;"
                            "对称 NAT 后面的对端将连不上";
        return; // 线程到此为止,iceServers() 永远返回兜底
    }

    while (true) {
        if (fetch(cfg)) {
            m_failures = 0;
        } else {
            ++m_failures;
            // 30s → 1m → 2m → … → 封顶 15 分钟
            const int backoff = std::min(kRetryBaseSeconds << std::min(m_failures - 1, 5),
                                         kRetryMaxSeconds);
            m_nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(backoff);
            LOG_WARN("Turn") << "第" << m_failures << "次获取 TURN 凭据失败,"
                             << backoff << "s 后重试";
        }

        std::unique_lock<std::mutex> lock(m_waitMutex);
        if (m_cv.wait_until(lock, m_nextAttempt, [this] { return m_stop; }))
            return; // 被 stop() 叫醒
    }
}

// ============================
// 兑换
// ============================

bool TurnCredentials::fetch(const ApiConfig &cfg) {
    const std::string target =
        "/v1/turn/keys/" + cfg.keyId + "/credentials/generate-ice-servers";

    beast::error_code ec;

    // 这条线程私有的 io_context:只服务这一次请求,用完即弃。
    // 不用服务端那个 io_context —— 这个类是同步阻塞的,不能被网络线程的
    // 事件循环搅在一起。
    asio::io_context ioc;

    ssl::context ctx(ssl::context::tls_client);

    // 系统 CA 目录(Debian 上是 /etc/ssl/certs)。直接调 OpenSSL 而不是
    // asio 的 set_default_verify_paths():后者那个重载的返回值就是传进去的 ec,
    // 白白多一个要接住的东西。这里返回的 1 是真的要看的结果。
    if (SSL_CTX_set_default_verify_paths(ctx.native_handle()) != 1) {
        LOG_ERROR("Turn") << "加载系统 CA 失败,无法校验证书";
        return false;
    }

    // 下一层用 beast::tcp_stream —— 只有它带 expires_after()。
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

    // SNI:少了它,按域名分流的负载均衡不知道该发哪张证书。
    if (SSL_set_tlsext_host_name(stream.native_handle(), kApiHost) != 1) {
        LOG_ERROR("Turn") << "设置 SNI 失败";
        return false;
    }

    // 证书链和主机名都要校。host_name_verification 是现在该用的那个
    // (老的 rfc2818_verification 已废弃)。
    stream.set_verify_mode(ssl::verify_peer);
    stream.set_verify_callback(ssl::host_name_verification(kApiHost));

    asio::ip::tcp::resolver resolver(ioc);
    const auto endpoints = resolver.resolve(kApiHost, kApiPort, ec);
    if (ec) {
        LOG_ERROR("Turn") << "解析 " << kApiHost << " 失败:" << ec.message();
        return false;
    }

    // 注意:expires_after 只对**下一次**操作生效,所以每个网络操作之前都要重新武装,
    // 否则只有第一个有超时,后面全变成"永远等下去"。
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(kRequestTimeoutSeconds));
    beast::get_lowest_layer(stream).connect(endpoints, ec);
    if (ec) {
        LOG_ERROR("Turn") << "连接 " << kApiHost << " 失败:" << ec.message();
        return false;
    }

    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(kRequestTimeoutSeconds));
    stream.handshake(ssl::stream_base::client, ec);
    if (ec) {
        LOG_ERROR("Turn") << "TLS 握手失败:" << ec.message();
        return false;
    }

    http::request<http::string_body> request{http::verb::post, target, 11};
    request.set(http::field::host, kApiHost);
    request.set(http::field::user_agent, "Syncine-Server/1.0");
    request.set(http::field::authorization, "Bearer " + cfg.apiToken);
    request.set(http::field::content_type, "application/json");
    request.body() = R"({"ttl":86400})";
    request.prepare_payload();

    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(kRequestTimeoutSeconds));
    http::write(stream, request, ec);
    if (ec) {
        LOG_ERROR("Turn") << "发送请求失败:" << ec.message();
        return false;
    }

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(kRequestTimeoutSeconds));
    http::read(stream, buffer, response, ec);
    if (ec) {
        LOG_ERROR("Turn") << "读取响应失败:" << ec.message();
        return false;
    }

    // 有序关闭 TLS。对端先关(asio::error::eof)或者没发 close_notify
    // (ssl::error::stream_truncated)都算正常 —— Beast 的 ssl_stream.hpp 顶部的
    // 说明里明确要求把这两个错误码当成功,别把它当多余代码删掉。
    // 响应这时候已经完整读到了,其余错误只记 debug。
    stream.shutdown(ec);
    if (ec && ec != asio::error::eof && ec != ssl::error::stream_truncated)
        LOG_DEBUG("Turn") << "TLS 关闭告警(已忽略):" << ec.message();

    if (response.result() != http::status::created && response.result() != http::status::ok) {
        // 401/403 = token 不对;404 = key id 不对。原文一并记下来,省得去猜。
        LOG_ERROR("Turn") << "凭据接口返回 " << response.result_int() << ":" << response.body();
        return false;
    }

    const json parsed = json::parse(response.body(), nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("iceServers")
        || !validateIceServers(parsed["iceServers"])) {
        LOG_ERROR("Turn") << "凭据接口返回了意料之外的 JSON:" << response.body();
        return false;
    }

    size_t serverCount = 0;
    size_t urlCount = 0;
    for (const json &entry : parsed["iceServers"]) {
        ++serverCount;
        urlCount += entry["urls"].size();
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_iceServers = parsed["iceServers"];
        m_validUntil = std::chrono::steady_clock::now() + std::chrono::seconds(kCredentialTtlSeconds);
    }

    const auto refreshIn = static_cast<long long>(kCredentialTtlSeconds * kRefreshAtRatio);
    m_nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(refreshIn);

    // 只记条数 —— username/credential 是临时密钥,任何级别都不写进日志。
    LOG_INFO("Turn") << "已获取 TURN 凭据:" << serverCount << " 组服务器 / " << urlCount
                     << " 个 URL," << refreshIn / 3600 << "h 后刷新";
    return true;
}

// ============================
// 对外
// ============================

json TurnCredentials::iceServers() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_iceServers.is_null() && std::chrono::steady_clock::now() < m_validUntil)
        return m_iceServers;

    // 从来没成功过,或者已经硬过期。过期凭据发下去只会让客户端白试一轮 TURN,
    // 不如直接给 STUN。
    return fallbackIceServers();
}
