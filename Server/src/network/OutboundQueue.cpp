#include "OutboundQueue.h"

#include <utility>

bool OutboundQueue::pushText(std::string payload, const Limits &limits) {
    return push({FrameKind::Text, std::move(payload)}, limits);
}

bool OutboundQueue::pushPing(const Limits &limits) {
    return push({FrameKind::Ping, {}}, limits);
}

std::optional<OutboundQueue::Frame> OutboundQueue::pop() {
    if (m_inflight || m_queued.empty())
        return std::nullopt;

    Frame frame = std::move(m_queued.front());
    m_queued.pop_front();
    m_queuedBytes -= frame.payload.size();
    m_inflightBytes = frame.payload.size();
    m_inflight = std::move(frame);
    return m_inflight;
}

void OutboundQueue::complete() {
    m_inflight.reset();
    m_inflightBytes = 0;
}

void OutboundQueue::clearQueued() {
    m_queued.clear();
    m_queuedBytes = 0;
}

bool OutboundQueue::empty() const {
    return totalMessages() == 0;
}

std::size_t OutboundQueue::totalMessages() const {
    return m_queued.size() + (m_inflight ? 1 : 0);
}

std::size_t OutboundQueue::totalBytes() const {
    return m_queuedBytes + m_inflightBytes;
}

bool OutboundQueue::push(Frame frame, const Limits &limits) {
    const std::size_t newBytes = frame.payload.size();
    if (totalMessages() >= limits.maxMessages)
        return false;
    if (totalBytes() + newBytes > limits.maxBytes)
        return false;

    m_queued.push_back(std::move(frame));
    m_queuedBytes += newBytes;
    return true;
}
