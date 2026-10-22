//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include "ServerOptions.h"

#include <memory>
#include <boost/asio.hpp>
namespace asio = boost::asio;

class WebSocketServer;

class Server {
public:
    Server(asio::io_context &ioContext, ServerOptions options);
    ~Server();
    // 监听建立失败（端口被占等）时返回 false，调用方必须终止进程
    bool start();
    void stop();
    unsigned short port() const;
    std::size_t connectionCount() const;

private:
    asio::io_context &m_ioc;
    ServerOptions m_options;
    std::unique_ptr<WebSocketServer> m_webSocketServer;
};


#endif //SERVER_SERVER_H
