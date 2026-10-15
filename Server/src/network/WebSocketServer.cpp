//
// Created by donggu on 2026/9/8.
//

#include "WebSocketServer.h"
#include "Log.h"
#include "Session.h"
#include "LogicSystem.h"

#include <vector>

WebSocketServer::WebSocketServer(asio::io_context &ioc, const std::string &address, unsigned short port) : m_ioc(ioc), m_acceptor(ioc) {
    asio::ip::tcp::endpoint endpoint(asio::ip::make_address(address), port);
    beast::error_code ec;

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
        LOG_FATAL("WebSocket") << "绑定 " << address << ":" << port << " 失败："
                               << ec.message();
        return;
    }

    m_acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        LOG_FATAL("WebSocket") << "开始监听失败：" << ec.message();
        return;
    }

    LOG_INFO("WebSocket") << "监听已就绪 " << address << ":" << port;
}

WebSocketServer::~WebSocketServer() {
    stop();
}

void WebSocketServer::run() {
    do_accept();
}

void WebSocketServer::stop() {
    LOG_INFO("WebSocket") << "正在停止服务，当前连接数=" << m_sessions.size();
    beast::error_code ec;
    m_acceptor.close(ec);
    if (ec) {
        LOG_ERROR("WebSocket") << "关闭监听套接字失败：" << ec.message();
    }

    // 关闭所有连接；先收集 id，避免关闭回调在遍历过程中修改 m_sessions
    std::vector<std::string> sessionIds;
    sessionIds.reserve(m_sessions.size());
    for (const auto &[id, session] : m_sessions)
        sessionIds.push_back(id);

    for (const std::string &id : sessionIds) {
        auto it = m_sessions.find(id);
        if (it != m_sessions.end())
            it->second->stop();
    }
    m_sessions.clear();

    LogicSystem::getInstance().stop();
    LOG_INFO("WebSocket") << "服务已停止";
}

void WebSocketServer::do_accept() {
    m_acceptor.async_accept(m_ioc, [this](beast::error_code ec, asio::ip::tcp::socket socket) {
        on_accept(ec, std::move(socket));
    });
}

void WebSocketServer::on_accept(beast::error_code ec, asio::ip::tcp::socket socket) {
    if (!ec) {
        auto session = std::make_shared<Session>(std::move(socket));
        const std::string sessionId = session->id();
        session->setOnClose([this](std::shared_ptr<Session> closed) {
            // 连接断开：从会话表移除，并让其退出所在房间
            LOG_INFO("WebSocket") << "连接关闭: clientId=" << closed->id()
                                  << " 剩余连接数=" << (m_sessions.size() - 1);
            m_sessions.erase(closed->id());
            LogicSystem::getInstance().handleDisconnect(closed);
        });
        m_sessions[sessionId] = session;
        LOG_INFO("WebSocket") << "新连接: clientId=" << sessionId
                              << " 当前连接数=" << m_sessions.size();
        session->start();
    } else if (ec != asio::error::operation_aborted) {
        LOG_ERROR("WebSocket") << "接受连接失败：" << ec.message();
    }

    // acceptor 仍打开则继续接受下一个连接
    if (m_acceptor.is_open())
        do_accept();
}
