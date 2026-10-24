#include "Session.h"

#include "Log.h"
#include "LogicSystem.h"
#include "RandomGen.h"

#include <nlohmann/json.hpp>

#include <utility>

Session::Session(asio::ip::tcp::socket &&socket, const ServerOptions &options,
                 LogicSystem &logic)
    : m_options(options),
      m_logic(logic),
      m_ws(std::make_unique<websocket::stream<asio::ip::tcp::socket>>(
          std::move(socket))),
      m_handshakeTimer(m_ws->get_executor()),
      m_heartbeatTimer(m_ws->get_executor()),
      m_pongTimer(m_ws->get_executor()),
      m_id(RandomGen::generateUUID()) {
}

Session::~Session() {
    cancelTimers();
    beast::error_code ec;
    m_ws->next_layer().close(ec);
}

void Session::start() {
    LOG_DEBUG("Session") << "会话开始: clientId=" << m_id
                         << " generation=" << generation();

    m_ws->read_message_max(m_options.maxMessageBytes);
    m_ws->control_callback(
        [this](websocket::frame_type kind, beast::string_view) {
            if (kind == websocket::frame_type::pong)
                onPong();
        });

    startHandshakeTimer();
    m_ws->async_accept([this, self = shared_from_this()](beast::error_code ec) {
        if (ec) {
            if (!m_closed.load(std::memory_order_acquire)) {
                LOG_WARN("Session") << "WebSocket 握手失败: clientId=" << m_id
                                    << " " << ec.message();
            }
            finishDisconnect(CloseReason::NetworkError);
            return;
        }

        m_handshakeTimer.cancel();

        nlohmann::json welcome;
        welcome["type"] = "welcome";
        welcome["clientId"] = m_id;
        send(welcome.dump());

        startHeartbeat();
        doRead();
    });
}

void Session::stop(CloseReason reason) {
    requestClose(reason);
}

bool Session::send(const std::string &message) {
    return queueFrame({OutboundQueue::FrameKind::Text, message});
}

const std::string &Session::id() const {
    return m_id;
}

std::uint64_t Session::generation() const {
    return m_generation.load(std::memory_order_acquire);
}

bool Session::isActiveGeneration(std::uint64_t generation) const {
    return !m_closed.load(std::memory_order_acquire)
        && m_generation.load(std::memory_order_acquire) == generation;
}

void Session::setOnClose(
    std::function<void(std::shared_ptr<Session>, CloseReason)> callback) {
    m_onClose = std::move(callback);
}

bool Session::queueFrame(OutboundQueue::Frame frame) {
    if (m_closed.load(std::memory_order_acquire)
        || m_closeRequested.load(std::memory_order_acquire)) {
        return false;
    }

    const OutboundQueue::Limits limits{
        .maxMessages = m_options.maxWriteQueueMessages,
        .maxBytes = m_options.maxWriteQueueBytes,
    };

    bool scheduleWriter = false;
    {
        std::lock_guard lock(m_outboundMutex);
        if (m_closed.load(std::memory_order_relaxed))
            return false;

        const bool accepted =
            frame.kind == OutboundQueue::FrameKind::Ping
                ? m_outbound.pushPing(limits)
                : m_outbound.pushText(std::move(frame.payload), limits);
        if (!accepted) {
            requestClose(CloseReason::OutboundQueueFull);
            return false;
        }

        if (!m_writerScheduled) {
            m_writerScheduled = true;
            scheduleWriter = true;
        }
    }

    if (scheduleWriter) {
        asio::post(m_ws->get_executor(),
                   [self = shared_from_this()]() { self->doNextWrite(); });
    }
    return true;
}

bool Session::queuePing() {
    return queueFrame({OutboundQueue::FrameKind::Ping, {}});
}

void Session::doNextWrite() {
    if (m_closed.load(std::memory_order_acquire))
        return;

    OutboundQueue::Frame frame;
    {
        std::lock_guard lock(m_outboundMutex);
        auto next = m_outbound.pop();
        if (!next) {
            m_writerScheduled = false;
            return;
        }
        frame = std::move(*next);
    }

    auto payload =
        std::make_shared<OutboundQueue::Frame>(std::move(frame));
    auto self = shared_from_this();

    if (payload->kind == OutboundQueue::FrameKind::Ping) {
        m_ws->async_ping(websocket::ping_data(payload->payload),
                         [this, self, payload](beast::error_code ec) {
            completeWrite();
            if (ec) {
                LOG_WARN("Session") << "心跳发送失败: clientId=" << m_id
                                    << " " << ec.message();
                finishDisconnect(CloseReason::NetworkError);
                return;
            }
            doNextWrite();
        });
        return;
    }

    m_ws->async_write(
        asio::buffer(payload->payload),
        [this, self, payload](beast::error_code ec, std::size_t) {
            completeWrite();
            if (ec) {
                LOG_WARN("Session") << "写入失败: clientId=" << m_id
                                    << " " << ec.message();
                finishDisconnect(CloseReason::NetworkError);
                return;
            }
            doNextWrite();
        });
}

void Session::completeWrite() {
    std::lock_guard lock(m_outboundMutex);
    m_outbound.complete();
}

void Session::doRead() {
    if (m_closed.load(std::memory_order_acquire))
        return;

    auto self = shared_from_this();
    m_ws->async_read(m_buffer,
                     [this, self](beast::error_code ec, std::size_t bytes) {
        if (ec) {
            const bool peerClosed = ec == websocket::error::closed
                                 || ec == asio::error::eof
                                 || ec == asio::error::connection_reset
                                 || ec == asio::error::connection_aborted;
            if (!peerClosed && ec != asio::error::operation_aborted) {
                LOG_WARN("Session") << "读取失败: clientId=" << m_id
                                    << " " << ec.message();
            }
            finishDisconnect(peerClosed ? CloseReason::PeerClosed
                                        : CloseReason::NetworkError);
            return;
        }

        if (m_ws->got_binary()) {
            finishDisconnect(CloseReason::ProtocolError);
            return;
        }

        const auto currentGeneration = generation();
        std::string message = beast::buffers_to_string(m_buffer.data());
        m_buffer.consume(bytes);

        const auto result = m_logic.enqueueMessage(
            self, currentGeneration, std::move(message));
        if (result == LogicSystem::EnqueueResult::Accepted) {
            doRead();
            return;
        }

        const CloseReason reason =
            result == LogicSystem::EnqueueResult::QueueFull
                ? CloseReason::InboundQueueFull
                : CloseReason::ServerStopping;
        finishDisconnect(reason);
    });
}

void Session::startHandshakeTimer() {
    m_handshakeTimer.expires_after(m_options.handshakeTimeout);
    m_handshakeTimer.async_wait(
        [self = shared_from_this()](beast::error_code ec) {
            if (ec || self->m_closed.load(std::memory_order_acquire))
                return;
            LOG_WARN("Session") << "WebSocket 握手超时: clientId=" << self->m_id;
            self->finishDisconnect(CloseReason::HandshakeTimeout);
        });
}

void Session::startHeartbeat() {
    if (m_options.heartbeatInterval.count() <= 0)
        return;

    m_heartbeatTimer.expires_after(m_options.heartbeatInterval);
    m_heartbeatTimer.async_wait(
        [self = shared_from_this()](beast::error_code ec) {
            if (ec || self->m_closed.load(std::memory_order_acquire)
                || self->m_closeRequested.load(std::memory_order_acquire)) {
                return;
            }

            if (!self->m_pingOutstanding) {
                if (!self->queuePing())
                    return;
                self->m_pingOutstanding = true;
                self->m_pongTimer.expires_after(
                    self->m_options.heartbeatTimeout);
                self->m_pongTimer.async_wait(
                    [self](beast::error_code timerError) {
                        if (timerError
                            || self->m_closed.load(std::memory_order_acquire)
                            || !self->m_pingOutstanding) {
                            return;
                        }
                        LOG_WARN("Session")
                            << "心跳超时: clientId=" << self->m_id;
                        self->finishDisconnect(
                            CloseReason::HeartbeatTimeout);
                    });
            }
            self->startHeartbeat();
        });
}

void Session::onPong() {
    if (!m_pingOutstanding)
        return;
    m_pingOutstanding = false;
    m_pongTimer.cancel();
}

void Session::requestClose(CloseReason reason) {
    if (m_closeRequested.exchange(true, std::memory_order_acq_rel))
        return;

    asio::post(m_ws->get_executor(),
               [self = shared_from_this(), reason]() {
        self->finishDisconnect(reason);
    });
}

void Session::finishDisconnect(CloseReason reason) {
    bool expected = false;
    if (!m_closed.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel))
        return;

    m_generation.fetch_add(1, std::memory_order_acq_rel);
    cancelTimers();

    beast::error_code ec;
    m_ws->next_layer().cancel(ec);
    ec.clear();
    m_ws->next_layer().close(ec);

    {
        std::lock_guard lock(m_outboundMutex);
        m_outbound.clearQueued();
        m_writerScheduled = false;
    }

    LOG_DEBUG("Session") << "会话结束: clientId=" << m_id
                         << " reason=" << closeReasonName(reason);
    if (m_onClose) {
        auto callback = m_onClose;
        callback(shared_from_this(), reason);
    }
}

void Session::cancelTimers() {
    m_handshakeTimer.cancel();
    m_heartbeatTimer.cancel();
    m_pongTimer.cancel();
}
