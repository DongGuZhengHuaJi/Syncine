//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include <memory>
#include <boost/asio.hpp>
namespace asio = boost::asio;

class WebSocketServer;

class Server {
public:
    Server(asio::io_context& ioContext, const std::string& address, unsigned short port);
    ~Server();
    // 监听建立失败（端口被占等）时返回 false，调用方必须终止进程
    bool start();
    void stop();

private:
    asio::io_context &m_ioc;
    std::unique_ptr<WebSocketServer> m_webSocketServer;
};


#endif //SERVER_SERVER_H
