//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_SESSION_H
#define SERVER_SESSION_H

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace websocket = beast::websocket; // from <boost/beast/websocket.hpp>
namespace asio = boost::asio;           // from <boost/asio.hpp>

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(asio::ip::tcp::socket &&socket);
    ~Session();

    void start();
    void stop();

    // 线程安全:worker 线程也可以直接调用
    void send(const std::string &message);

    std::string id() const;

    // 连接关闭时回调(由 WebSocketServer 设置,用于清理会话表和房间)
    void setOnClose(std::function<void(std::shared_ptr<Session>)> callback);

private:
    void do_read();
    void do_next_write();
    void onDisconnected();

    std::unique_ptr<websocket::stream<asio::ip::tcp::socket>> m_ws;
    beast::flat_buffer m_buffer;

    // 发送队列只在 io_context 线程里操作(所有写入都 post 到它),无需加锁
    std::deque<std::string> m_writeQueue;
    bool m_writing = false;

    const std::string m_id;
    std::function<void(std::shared_ptr<Session>)> m_onClose;
    bool m_closed = false;
};

#endif //SERVER_SESSION_H
