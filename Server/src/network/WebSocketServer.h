#ifndef SERVER_WEBSOCKETSERVER_H
#define SERVER_WEBSOCKETSERVER_H

#include "LogicSystem.h"
#include "ServerOptions.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <chrono>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

namespace beast = boost::beast;
namespace asio = boost::asio;

class Session;

class WebSocketServer {
public:
    WebSocketServer(asio::io_context &ioc, ServerOptions options);
    ~WebSocketServer();

    void run();
    void stop();

    bool isListening() const {
        return m_listening;
    }

    unsigned short port() const {
        return m_port;
    }

    std::size_t connectionCount() const {
        return m_connectionCount.load(std::memory_order_acquire);
    }

private:
    void doAccept();
    void onAccept(beast::error_code ec, asio::ip::tcp::socket socket);
    void scheduleAcceptRetry();
    void onSessionClosed(const std::shared_ptr<Session> &session, CloseReason reason);

    asio::io_context &m_ioc;
    ServerOptions m_options;
    asio::ip::tcp::acceptor m_acceptor;
    asio::steady_timer m_acceptRetryTimer;
    LogicSystem m_logic;
    std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;
    std::atomic<std::size_t> m_connectionCount{0};

    bool m_listening = false;
    bool m_accepting = false;
    bool m_stopping = false;
    unsigned short m_port = 0;
    std::chrono::milliseconds m_acceptBackoff{0};
};

#endif // SERVER_WEBSOCKETSERVER_H
