#include "WebSocketServer.h"

#include "Log.h"
#include "Session.h"

#include <algorithm>
#include <utility>
#include <vector>

WebSocketServer::WebSocketServer(asio::io_context &ioc, ServerOptions options)
    : m_ioc(ioc),
      m_options(std::move(options)),
      m_acceptor(ioc),
      m_acceptRetryTimer(ioc),
      m_logic(m_options.inboundQueue, m_options.logicThreads),
      m_acceptBackoff(m_options.acceptBackoffInitial) {
    beast::error_code ec;
    const auto address = asio::ip::make_address(m_options.address, ec);
    if (ec) {
        LOG_FATAL("WebSocket") << "监听地址无效 " << m_options.address
                               << "：" << ec.message();
        return;
    }

    const asio::ip::tcp::endpoint endpoint(address, m_options.port);
    m_acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        LOG_FATAL("WebSocket") << "打开监听套接字失败：" << ec.message();
        return;
    }

    m_acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        LOG_ERROR("WebSocket") << "设置 reuse_address 失败：" << ec.message();
        return;
    }

    m_acceptor.bind(endpoint, ec);
    if (ec) {
        LOG_FATAL("WebSocket") << "绑定 " << m_options.address << ":"
                               << m_options.port << " 失败：" << ec.message();
        return;
    }

    m_acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        LOG_FATAL("WebSocket") << "开始监听失败：" << ec.message();
        return;
    }

    m_port = m_acceptor.local_endpoint(ec).port();
    if (ec) {
        LOG_ERROR("WebSocket") << "读取监听端口失败：" << ec.message();
        m_port = m_options.port;
        ec.clear();
    }

    m_listening = true;
    LOG_INFO("WebSocket") << "监听已就绪 " << m_options.address << ":"
                          << m_port << " 最大连接数="
                          << m_options.maxConnections;
}

WebSocketServer::~WebSocketServer() {
    stop();
}

void WebSocketServer::run() {
    if (!m_listening) {
        LOG_FATAL("WebSocket") << "监听未建立，放弃接受连接";
        return;
    }
    doAccept();
}

void WebSocketServer::stop() {
    if (m_stopping)
        return;
    m_stopping = true;

    LOG_INFO("WebSocket") << "正在停止服务，当前连接数=" << m_sessions.size();
    beast::error_code ec;
    m_acceptRetryTimer.cancel();
    m_acceptor.close(ec);
    if (ec) {
        LOG_ERROR("WebSocket") << "关闭监听套接字失败：" << ec.message();
    }

    std::vector<std::shared_ptr<Session>> sessions;
    sessions.reserve(m_sessions.size());
    for (const auto &[id, session] : m_sessions) {
        (void)id;
        sessions.push_back(session);
    }
    m_sessions.clear();
    m_connectionCount.store(0, std::memory_order_release);

    for (const auto &session : sessions)
        session->stop(CloseReason::ServerStopping);

    m_logic.stop();
    LOG_INFO("WebSocket") << "服务已停止";
}

void WebSocketServer::doAccept() {
    if (m_stopping || !m_listening || m_accepting
        || m_sessions.size() >= m_options.maxConnections) {
        LOG_DEBUG("WebSocket") << "暂缓 accept: stopping=" << (m_stopping ? 1 : 0)
                               << " listening=" << (m_listening ? 1 : 0)
                               << " accepting=" << (m_accepting ? 1 : 0)
                               << " sessions=" << m_sessions.size()
                               << " max=" << m_options.maxConnections;
        return;
    }

    LOG_DEBUG("WebSocket") << "准备 accept: sessions=" << m_sessions.size();
    m_accepting = true;
    m_acceptor.async_accept(
        m_ioc, [this](beast::error_code ec, asio::ip::tcp::socket socket) {
            onAccept(ec, std::move(socket));
        });
}

void WebSocketServer::onAccept(beast::error_code ec,
                               asio::ip::tcp::socket socket) {
    m_accepting = false;

    if (ec) {
        if (!m_stopping && ec != asio::error::operation_aborted)
            scheduleAcceptRetry();
        return;
    }

    m_acceptBackoff = m_options.acceptBackoffInitial;
    if (m_stopping || m_sessions.size() >= m_options.maxConnections) {
        beast::error_code closeError;
        socket.close(closeError);
        LOG_WARN("WebSocket") << "拒绝连接: reason="
                              << closeReasonName(CloseReason::ConnectionLimit);
        doAccept();
        return;
    }

    auto session =
        std::make_shared<Session>(std::move(socket), m_options, m_logic);
    const std::string sessionId = session->id();
    session->setOnClose(
        [this](std::shared_ptr<Session> closed, CloseReason reason) {
            onSessionClosed(closed, reason);
        });
    m_sessions.emplace(sessionId, session);
    m_connectionCount.fetch_add(1, std::memory_order_acq_rel);

    LOG_INFO("WebSocket") << "新连接: clientId=" << sessionId
                          << " generation=" << session->generation()
                          << " 当前连接数=" << m_sessions.size();
    session->start();
    doAccept();
}

void WebSocketServer::scheduleAcceptRetry() {
    if (m_stopping)
        return;

    const auto delay = std::clamp(
        m_acceptBackoff, m_options.acceptBackoffInitial,
        m_options.acceptBackoffMax);
    LOG_ERROR("WebSocket") << "接受连接失败，将在 " << delay.count()
                           << "ms 后重试";

    m_acceptRetryTimer.expires_after(delay);
    m_acceptRetryTimer.async_wait([this](beast::error_code ec) {
        if (!ec && !m_stopping)
            doAccept();
    });

    m_acceptBackoff = std::min(
        m_options.acceptBackoffMax,
        std::max(m_options.acceptBackoffInitial, m_acceptBackoff * 2));
}

void WebSocketServer::onSessionClosed(
    const std::shared_ptr<Session> &session, CloseReason reason) {
    const auto it = m_sessions.find(session->id());
    if (it == m_sessions.end())
        return;
    m_sessions.erase(it);
    m_connectionCount.fetch_sub(1, std::memory_order_acq_rel);

    LOG_INFO("WebSocket") << "连接关闭: clientId=" << session->id()
                          << " reason=" << closeReasonName(reason)
                          << " 剩余连接数=" << m_sessions.size();
    m_logic.enqueueDisconnect(session, reason);

    if (!m_stopping && m_sessions.size() < m_options.maxConnections)
        doAccept();
}
