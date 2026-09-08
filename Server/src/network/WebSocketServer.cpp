//
// Created by donggu on 2026/9/8.
//

#include "WebSocketServer.h"
#include "Logger.h"
#include "Session.h"
#include "LogicSystem.h"

#include <vector>

WebSocketServer::WebSocketServer(asio::io_context &ioc, const std::string &address, unsigned short port) : m_ioc(ioc), m_acceptor(ioc) {
    asio::ip::tcp::endpoint endpoint(asio::ip::make_address(address), port);
    beast::error_code ec;

    m_acceptor.open(endpoint.protocol(), ec);
    if (ec) {
        Logger::error("Error opening acceptor: " + ec.message());
        return;
    }

    m_acceptor.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        Logger::error("Error setting socket option: " + ec.message());
        return;
    }

    m_acceptor.bind(endpoint, ec);
    if (ec) {
        Logger::error("Error binding acceptor: " + ec.message());
        return;
    }

    m_acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        Logger::error("Error listening on acceptor: " + ec.message());
        return;
    }

    Logger::info("WebSocket server initialized on " + address + ":" + std::to_string(port));
}

WebSocketServer::~WebSocketServer() {
    stop();
}

void WebSocketServer::run() {
    do_accept();
}

void WebSocketServer::stop() {
    Logger::info("Stopping WebSocket server...");
    beast::error_code ec;
    m_acceptor.close(ec);
    if (ec) {
        Logger::error("Error closing acceptor: " + ec.message());
    }

    // 关闭所有连接;先收集 id,避免关闭回调在遍历中修改 m_sessions
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
            // 连接断开:从会话表移除,并让它退出所在房间
            m_sessions.erase(closed->id());
            LogicSystem::getInstance().handleDisconnect(closed);
        });
        m_sessions[sessionId] = session;
        session->start();
    } else if (ec != asio::error::operation_aborted) {
        Logger::error("Accept error: " + ec.message());
    }

    // acceptor 仍打开就继续接受下一个连接
    if (m_acceptor.is_open())
        do_accept();
}
