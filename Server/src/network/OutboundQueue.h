#ifndef SERVER_OUTBOUNDQUEUE_H
#define SERVER_OUTBOUNDQUEUE_H

#include <cstddef>
#include <deque>
#include <optional>
#include <string>

class OutboundQueue {
public:
    enum class FrameKind {
        Text,
        Ping,
    };

    struct Frame {
        FrameKind kind = FrameKind::Text;
        std::string payload;
    };

    struct Limits {
        std::size_t maxMessages = 256;
        std::size_t maxBytes = 8 * 1024 * 1024;
    };

    bool pushText(std::string payload, const Limits &limits);
    bool pushPing(const Limits &limits);
    std::optional<Frame> pop();
    void complete();
    void clearQueued();

    bool empty() const;
    std::size_t totalMessages() const;
    std::size_t totalBytes() const;

private:
    bool push(Frame frame, const Limits &limits);

    std::deque<Frame> m_queued;
    std::optional<Frame> m_inflight;
    std::size_t m_queuedBytes = 0;
    std::size_t m_inflightBytes = 0;
};

#endif // SERVER_OUTBOUNDQUEUE_H

