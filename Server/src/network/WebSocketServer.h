//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_WEBSOCKETSERVER_H
#define SERVER_WEBSOCKETSERVER_H

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <memory>
#include <string>
#include <unordered_map>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace asio = boost::asio;           // from <boost/asio.hpp>

class Session;

class WebSocketServer {
public:
    WebSocketServer(asio::io_context& ioc, const std::string& address, unsigned short port);
    ~WebSocketServer();

    void run();
    void stop();

private:
    void do_accept();
    void on_accept(beast::error_code ec, asio::ip::tcp::socket socket);

    asio::io_context& m_ioc;
    asio::ip::tcp::acceptor m_acceptor;

    // 会话表:负责持有 Session 的生命周期
    std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;
};

#endif //SERVER_WEBSOCKETSERVER_H
