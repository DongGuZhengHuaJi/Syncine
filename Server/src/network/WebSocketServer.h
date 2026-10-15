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

    // 监听是否真的建立起来了。构造过程中 open/bind/listen 任一步失败都会让它
    // 保持 false —— 此时必须由调用方终止进程，不能继续跑（见 .cpp 的说明）
    bool isListening() const { return m_listening; }

private:
    void do_accept();
    void on_accept(beast::error_code ec, asio::ip::tcp::socket socket);

    asio::io_context& m_ioc;
    asio::ip::tcp::acceptor m_acceptor;

    // 只有 listen() 成功才置 true。
    // 不能用 m_acceptor.is_open() 代替：open() 成功而 bind() 失败时它仍是
    // true，于是 accept 循环会在一个没绑上的套接字上无限重试
    bool m_listening = false;

    // 会话表：负责持有 Session 的生命周期
    std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;
};

#endif //SERVER_WEBSOCKETSERVER_H
