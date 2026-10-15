//
// Created by donggu on 2026/9/8.
//

#include "Server.h"
#include "Log.h"
#include "Config.h"
#include "WebSocketServer.h"

Server::Server(asio::io_context& ioContext, const std::string& address, unsigned short port)
    : m_ioc(ioContext),
      m_webSocketServer(std::make_unique<WebSocketServer>(m_ioc, address, port)) {
}

Server::~Server() {
    stop();
}

bool Server::start() {
    // 必须先确认监听真的建立了再报「服务启动」。
    // 端口被占时旧代码照样打印这一行并进入 accept 循环，结果是在一个没绑上
    // 的套接字上自旋 —— 实测进程状态 R、单核跑满，而 systemd 完全看不出问题。
    if (!m_webSocketServer->isListening()) {
        LOG_FATAL("Server") << "监听未建立，服务无法启动 "
                            << Config::SERVER_ADDRESS << ":" << Config::SERVER_PORT;
        return false;
    }

    LOG_INFO("Server") << "服务启动 " << Config::SERVER_ADDRESS
                       << ":" << Config::SERVER_PORT;
    m_webSocketServer->run();
    return true;
}

void Server::stop() {
    LOG_INFO("Server") << "服务停止";
    m_webSocketServer->stop();
}
