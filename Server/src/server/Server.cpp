//
// Created by donggu on 2026/9/8.
//

#include "Server.h"
#include "Logger.h"
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
    Logger::info("Starting server on " + Config::SERVER_ADDRESS + ":" + std::to_string(Config::SERVER_PORT));
    m_webSocketServer->run();
}

void Server::stop() {
    Logger::info("Stopping server...");
    m_webSocketServer->stop();
}
