#ifndef SERVER_SERVEROPTIONS_H
#define SERVER_SERVEROPTIONS_H

#include <chrono>
#include <cstddef>
#include <string>

struct InboundQueueLimits {
    std::size_t maxSessionTasks = 64;
    std::size_t maxSessionBytes = 1024 * 1024;
    std::size_t maxGlobalTasks = 4096;
    std::size_t maxGlobalBytes = 16 * 1024 * 1024;
};

struct ServerOptions {
    std::string address = "0.0.0.0";
    unsigned short port = 8765;
    std::size_t logicThreads = 0;

    std::size_t maxConnections = 1024;
    InboundQueueLimits inboundQueue;
    std::size_t maxWriteQueueMessages = 256;
    std::size_t maxWriteQueueBytes = 8 * 1024 * 1024;
    std::size_t maxMessageBytes = 64 * 1024;

    std::chrono::milliseconds handshakeTimeout{5000};
    std::chrono::milliseconds heartbeatInterval{30000};
    std::chrono::milliseconds heartbeatTimeout{60000};
    std::chrono::milliseconds acceptBackoffInitial{100};
    std::chrono::milliseconds acceptBackoffMax{5000};
};

#endif // SERVER_SERVEROPTIONS_H

