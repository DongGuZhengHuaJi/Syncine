#ifndef SERVER_CLOSEREASON_H
#define SERVER_CLOSEREASON_H

enum class CloseReason {
    PeerClosed,
    ProtocolError,
    NetworkError,
    HandshakeTimeout,
    HeartbeatTimeout,
    InboundQueueFull,
    OutboundQueueFull,
    ConnectionLimit,
    ServerStopping,
};

inline const char *closeReasonName(CloseReason reason) {
    switch (reason) {
    case CloseReason::PeerClosed:
        return "peer_closed";
    case CloseReason::ProtocolError:
        return "protocol_error";
    case CloseReason::NetworkError:
        return "network_error";
    case CloseReason::HandshakeTimeout:
        return "handshake_timeout";
    case CloseReason::HeartbeatTimeout:
        return "heartbeat_timeout";
    case CloseReason::InboundQueueFull:
        return "inbound_queue_full";
    case CloseReason::OutboundQueueFull:
        return "outbound_queue_full";
    case CloseReason::ConnectionLimit:
        return "connection_limit";
    case CloseReason::ServerStopping:
        return "server_stopping";
    }
    return "unknown";
}

#endif // SERVER_CLOSEREASON_H

