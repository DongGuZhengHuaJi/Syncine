#include "TestHarness.h"

#include "network/OutboundQueue.h"

#include <string>

TEST_CASE(outbound_queue_counts_queued_and_inflight_frames) {
    OutboundQueue queue;
    const OutboundQueue::Limits limits{
        .maxMessages = 2,
        .maxBytes = 8,
    };

    CHECK(queue.pushText("1234", limits));
    CHECK(queue.totalMessages() == 1);
    CHECK(queue.totalBytes() == 4);

    auto first = queue.pop();
    CHECK(first.has_value());
    CHECK(first->kind == OutboundQueue::FrameKind::Text);
    CHECK(first->payload == "1234");
    CHECK(queue.totalMessages() == 1);

    CHECK(queue.pushPing(limits));
    CHECK(queue.totalMessages() == 2);
    CHECK(!queue.pushText("x", limits));

    queue.complete();
    CHECK(queue.totalMessages() == 1);
    CHECK(queue.totalBytes() == 0);

    auto second = queue.pop();
    CHECK(second.has_value());
    CHECK(second->kind == OutboundQueue::FrameKind::Ping);
    queue.complete();
    CHECK(queue.totalMessages() == 0);
}

TEST_CASE(outbound_queue_rejects_oversized_frame) {
    OutboundQueue queue;
    const OutboundQueue::Limits limits{
        .maxMessages = 8,
        .maxBytes = 4,
    };

    CHECK(!queue.pushText("12345", limits));
    CHECK(queue.empty());
}

TEST_CASE(outbound_queue_clears_only_queued_frames) {
    OutboundQueue queue;
    const OutboundQueue::Limits limits{
        .maxMessages = 8,
        .maxBytes = 64,
    };

    CHECK(queue.pushText("first", limits));
    CHECK(queue.pushText("second", limits));
    CHECK(queue.pop().has_value());
    queue.clearQueued();

    CHECK(queue.totalMessages() == 1);
    CHECK(queue.totalBytes() == 5);
    queue.complete();
    CHECK(queue.totalMessages() == 0);
}

