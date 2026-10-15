//
// Created by donggu on 2026/10/14.
//
// 见 TurnCredentials.h 顶部说明（类的用途与线程模型）。
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

// Secrets.h 不进仓库（见 .gitignore）。没有它的机器改走环境变量；
// 两者皆不可用时仍可运行，只是没有 TURN 中继（详见 run()）。
#if __has_include("Secrets.h")
#  include "Secrets.h"
#endif

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;

using json = nlohmann::json;

namespace {

// Cloudflare 允许的最长 ttl 即 24 小时，更长申请不到。
constexpr long kCredentialTtlSeconds = 86400;

// 使用 ttl 的这一比例后提前刷新。留出 1/4（6 小时）作为重试窗口：
// 最长退避 15 分钟，该窗口内可重试二十余次。
constexpr double kRefreshAtRatio = 0.75;

constexpr int kRetryBaseSeconds = 30;
constexpr int kRetryMaxSeconds = 900; // 15 分钟
constexpr int kRequestTimeoutSeconds = 15;
constexpr const char *kApiHost = "rtc.live.cloudflare.com";
constexpr const char *kApiPort = "443";

// 无可用凭据时的兜底：一个公共 STUN，局域网与普通 NAT 仍可直连。
//
// 不可写成带 query 的 stun URL（如 ?transport=tcp）：libwebrtc 的 ICE URL 解析
// 拒绝带 query 的 stun URL，且属硬错误——任一 URL 非法即整条 PeerConnection
// 无法建立（不会跳过该项），客户端仅报一句连接创建失败，极难排查。
json fallbackIceServers() {
    json stun;
    stun["urls"] = json::array({"stun:stun.cloudflare.com:3478"});
    return json::array({stun});
}

// 下发前校验形状。理由同上：坏配置会使客户端整条连接失败，在此拦截并退回
// STUN 兜底更好——少一个中继，但连接仍能建立。
bool validateIceServers(const json &list) {
    if (!list.is_array() || list.empty()) {
        LOG_ERROR("Turn") << "ICE 配置校验失败，拒绝下发：不是非空数组";
        return false;
    }

    for (const json &entry : list) {
        if (!entry.is_object() || !entry.contains("urls") || !entry["urls"].is_array()
            || entry["urls"].empty()) {
            LOG_ERROR("Turn") << "ICE 配置校验失败，拒绝下发：某一项缺少 urls";
            return false;
        }

        bool hasTurn = false;
        for (const json &url : entry["urls"]) {
            if (!url.is_string() || url.get<std::string>().empty()) {
                LOG_ERROR("Turn") << "ICE 配置校验失败，拒绝下发：url 不是非空字符串";
                return false;
            }

            const std::string text = url.get<std::string>();
            const bool isStun = text.rfind("stun", 0) == 0;

            // stun URL 带 query 会被 libwebrtc 判为非法（见 fallbackIceServers 说明）
            if (isStun && text.find('?') != std::string::npos) {
                LOG_ERROR("Turn") << "ICE 配置校验失败，拒绝下发：STUN url 带 query"
                                     "（会导致整条连接无法建立）：" << text;
                return false;
            }
            if (!isStun && text.rfind("turn", 0) == 0)
                hasTurn = true;
        }

        // TURN 项必须有凭据：username/credential 为空同样是硬错误
        const bool hasUsername = entry.contains("username") && entry["username"].is_string()
                                 && !entry["username"].get<std::string>().empty();
        const bool hasCredential = entry.contains("credential")
                                   && entry["credential"].is_string()
                                   && !entry["credential"].get<std::string>().empty();
        if (hasTurn && (!hasUsername || !hasCredential)) {
            LOG_ERROR("Turn") << "ICE 配置校验失败，拒绝下发：TURN 项缺少 username/credential";
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
    // 函数内 static 初始化自 C++11 起线程安全。注意不要在静态初始化阶段访问。
    static TurnCredentials instance;
    return instance;
}

TurnCredentials::~TurnCredentials() {
    stop(); // 兜底；正常路径是 main 中显式 stop，那时 Log 仍在运行
}

// ============================
// 账户密钥
// ============================

// 环境变量优先，其次 Secrets.h。此优先级是有意为之：
// 换用另一套 key 试用时无需改文件、无需重新编译。
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
        LOG_INFO("Turn") << "TURN 账户密钥来源：" << (fromEnv ? "环境变量" : "Secrets.h");

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
            return; // 幂等：重复调用直接返回（析构函数中还会再调一次）
        m_started = false;
        m_stop = true;
    }

    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join(); // 最坏情况等待一次请求完成（以 kRequestTimeoutSeconds 封顶）

    LOG_INFO("Turn") << "TURN 凭据服务已停止";
}

// ============================
// 后台线程
// ============================

void TurnCredentials::run() {
    Log::nameCurrentThread("turn");

    const ApiConfig cfg = loadApiConfig();
    if (!cfg.complete()) {
        // 不是致命错误：STUN 无需鉴权，缺少 TURN 只是对称 NAT 后的对端连不上。
        // 打一行 WARN 说明即可，不应每次进房都刷 ERROR。
        LOG_WARN("Turn") << "未配置 TURN 密钥（Secrets.h 或 SYNCINE_TURN_KEY_ID / "
                            "SYNCINE_TURN_API_TOKEN），仅下发 STUN；"
                            "对称 NAT 后的对端将无法连接";
        return; // 线程到此结束，iceServers() 永远返回兜底值
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
            LOG_WARN("Turn") << "获取 TURN 凭据失败第 " << m_failures << " 次，"
                             << backoff << "s 后重试";
        }

        std::unique_lock<std::mutex> lock(m_waitMutex);
        if (m_cv.wait_until(lock, m_nextAttempt, [this] { return m_stop; }))
            return; // 被 stop() 唤醒
    }
}

// ============================
// 兑换
// ============================

bool TurnCredentials::fetch(const ApiConfig &cfg) {
    const std::string target =
        "/v1/turn/keys/" + cfg.keyId + "/credentials/generate-ice-servers";

    beast::error_code ec;

    // 线程私有的 io_context：只服务本次请求，用完即弃。不使用服务端的那个——
    // 本类是同步阻塞的，不能与网络线程的事件循环混在一起。
    asio::io_context ioc;

    ssl::context ctx(ssl::context::tls_client);

    // 加载系统 CA 目录（Debian 上为 /etc/ssl/certs）。直接调 OpenSSL 而非 asio 的
    // set_default_verify_paths()：后者该重载的返回值即传入的 ec，多一个无用的
    // 接收点；此处返回的 1 才是真正需要判断的结果。
    if (SSL_CTX_set_default_verify_paths(ctx.native_handle()) != 1) {
        LOG_ERROR("Turn") << "加载系统 CA 失败，无法校验证书";
        return false;
    }

    // 下一层用 beast::tcp_stream —— 只有它带 expires_after()
    beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);

    // SNI：缺少它时，按域名分流的负载均衡无法确定返回哪张证书
    if (SSL_set_tlsext_host_name(stream.native_handle(), kApiHost) != 1) {
        LOG_ERROR("Turn") << "设置 SNI 失败";
        return false;
    }

    // 证书链与主机名均需校验。host_name_verification 为当前推荐用法
    // （rfc2818_verification 已废弃）。
    stream.set_verify_mode(ssl::verify_peer);
    stream.set_verify_callback(ssl::host_name_verification(kApiHost));

    asio::ip::tcp::resolver resolver(ioc);
    const auto endpoints = resolver.resolve(kApiHost, kApiPort, ec);
    if (ec) {
        LOG_ERROR("Turn") << "解析 " << kApiHost << " 失败：" << ec.message();
        return false;
    }

    // 注意：expires_after 只对下一次操作生效，因此每个网络操作之前都需重新设置，
    // 否则只有第一个操作带超时，后续操作会无限等待
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(kRequestTimeoutSeconds));
    beast::get_lowest_layer(stream).connect(endpoints, ec);
    if (ec) {
        LOG_ERROR("Turn") << "连接 " << kApiHost << " 失败：" << ec.message();
        return false;
    }

    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(kRequestTimeoutSeconds));
    stream.handshake(ssl::stream_base::client, ec);
    if (ec) {
        LOG_ERROR("Turn") << "TLS 握手失败：" << ec.message();
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
        LOG_ERROR("Turn") << "发送请求失败：" << ec.message();
        return false;
    }

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(kRequestTimeoutSeconds));
    http::read(stream, buffer, response, ec);
    if (ec) {
        LOG_ERROR("Turn") << "读取响应失败：" << ec.message();
        return false;
    }

    // 有序关闭 TLS。对端先关闭（asio::error::eof）或未发送 close_notify
    // （ssl::error::stream_truncated）均属正常——Beast 的 ssl_stream.hpp 顶部说明
    // 明确要求将这两个错误码视为成功，不可当作冗余代码删除。此时响应已完整读取，
    // 其余错误仅记 debug。
    stream.shutdown(ec);
    if (ec && ec != asio::error::eof && ec != ssl::error::stream_truncated)
        LOG_DEBUG("Turn") << "TLS 关闭告警（已忽略）：" << ec.message();

    if (response.result() != http::status::created && response.result() != http::status::ok) {
        // 401/403 为 token 错误，404 为 key id 错误。一并记录原文，避免事后猜测。
        LOG_ERROR("Turn") << "凭据接口返回 " << response.result_int() << "：" << response.body();
        return false;
    }

    const json parsed = json::parse(response.body(), nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("iceServers")
        || !validateIceServers(parsed["iceServers"])) {
        LOG_ERROR("Turn") << "凭据接口返回了预期之外的 JSON：" << response.body();
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

    // 只记录条数：username/credential 是临时密钥，任何级别都不写入日志
    LOG_INFO("Turn") << "已获取 TURN 凭据：" << serverCount << " 组服务器 / " << urlCount
                     << " 个 URL，" << refreshIn / 3600 << "h 后刷新";
    return true;
}

// ============================
// 对外
// ============================

json TurnCredentials::iceServers() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_iceServers.is_null() && std::chrono::steady_clock::now() < m_validUntil)
        return m_iceServers;

    // 从未成功过，或已硬过期。下发过期凭据只会让客户端白试一轮 TURN，不如直接给 STUN。
    return fallbackIceServers();
}
