#ifndef SERVER_SESSION_H
#define SERVER_SESSION_H

#include "CloseReason.h"
#include "OutboundQueue.h"
#include "ServerOptions.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace asio = boost::asio;

class LogicSystem;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(asio::ip::tcp::socket &&socket, const ServerOptions &options,
            LogicSystem &logic);
    ~Session();

    void start();
    void stop(CloseReason reason = CloseReason::ServerStopping);

    // Thread-safe. Returns false when the connection is closing or the
    // outbound queue would exceed its hard limit.
    bool send(const std::string &message);

    const std::string &id() const;
    std::uint64_t generation() const;
    bool isActiveGeneration(std::uint64_t generation) const;

    void setOnClose(
        std::function<void(std::shared_ptr<Session>, CloseReason)> callback);

private:
    bool queueFrame(OutboundQueue::Frame frame);
    bool queuePing();
    void doNextWrite();
    void completeWrite();
    void doRead();
    void startHandshakeTimer();
    void startHeartbeat();
    void onPong();
    void requestClose(CloseReason reason);
    void finishDisconnect(CloseReason reason);
    void cancelTimers();

    ServerOptions m_options;
    LogicSystem &m_logic;
    std::unique_ptr<websocket::stream<asio::ip::tcp::socket>> m_ws;
    beast::flat_buffer m_buffer;

    asio::steady_timer m_handshakeTimer;
    asio::steady_timer m_heartbeatTimer;
    asio::steady_timer m_pongTimer;
    bool m_pingOutstanding = false;

    std::mutex m_outboundMutex;
    OutboundQueue m_outbound;
    bool m_writerScheduled = false;

    const std::string m_id;
    std::atomic<std::uint64_t> m_generation{1};
    std::atomic<bool> m_closed{false};
    std::atomic<bool> m_closeRequested{false};
    std::function<void(std::shared_ptr<Session>, CloseReason)> m_onClose;
};

#endif // SERVER_SESSION_H
