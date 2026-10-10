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

void Server::start() {
    LOG_INFO("Server") << "Starting server on " << Config::SERVER_ADDRESS
                       << ":" << Config::SERVER_PORT;
    m_webSocketServer->run();
}

void Server::stop() {
    LOG_INFO("Server") << "Stopping server...";
    m_webSocketServer->stop();
}
