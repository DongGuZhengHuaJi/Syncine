#include "TestHarness.h"

#include "network/SerialTaskQueue.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

SerialTaskQueue::Limits smallLimits() {
    return {
        .maxSessionTasks = 32,
        .maxSessionBytes = 1024,
        .maxGlobalTasks = 64,
        .maxGlobalBytes = 4096,
    };
}

void waitUntil(const std::function<bool()> &predicate,
               std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(2ms);
    }
}

} // namespace

TEST_CASE(serial_queue_preserves_order_for_one_key) {
    SerialTaskQueue queue(2, smallLimits());
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<int> values;

    for (int i = 0; i < 20; ++i) {
        const auto result = queue.enqueue("session-a", 1, [&, i]() {
            {
                std::lock_guard lock(mutex);
                values.push_back(i);
            }
            cv.notify_all();
        });
        CHECK(result == SerialTaskQueue::EnqueueResult::Accepted);
    }

    waitUntil([&]() {
        std::lock_guard lock(mutex);
        return values.size() == 20;
    });

    for (int i = 0; i < 20; ++i)
        CHECK(values[static_cast<std::size_t>(i)] == i);
}

TEST_CASE(serial_queue_runs_different_keys_in_parallel) {
    SerialTaskQueue queue(2, smallLimits());
    std::atomic<bool> secondStarted{false};
    std::atomic<bool> firstFinished{false};
    std::atomic<bool> secondFinished{false};

    CHECK(queue.enqueue("session-a", 1, [&]() {
        waitUntil([&]() { return secondStarted.load(); });
        firstFinished.store(true);
    }) == SerialTaskQueue::EnqueueResult::Accepted);

    CHECK(queue.enqueue("session-b", 1, [&]() {
        secondStarted.store(true);
        secondFinished.store(true);
    }) == SerialTaskQueue::EnqueueResult::Accepted);

    waitUntil([&]() {
        return firstFinished.load() && secondFinished.load();
    });
}

TEST_CASE(serial_queue_enforces_session_and_global_limits) {
    auto limits = smallLimits();
    limits.maxSessionTasks = 1;
    limits.maxSessionBytes = 8;
    limits.maxGlobalTasks = 2;
    limits.maxGlobalBytes = 16;
    SerialTaskQueue queue(1, limits);

    std::mutex mutex;
    std::condition_variable cv;
    bool release = false;
    bool started = false;

    CHECK(queue.enqueue("session-a", 1, [&]() {
        std::unique_lock lock(mutex);
        started = true;
        cv.notify_all();
        cv.wait(lock, [&]() { return release; });
    }) == SerialTaskQueue::EnqueueResult::Accepted);

    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, 1s, [&]() { return started; }));
    }

    CHECK(queue.enqueue("session-a", 1, []() {})
          == SerialTaskQueue::EnqueueResult::Accepted);
    CHECK(queue.enqueue("session-a", 1, []() {})
          == SerialTaskQueue::EnqueueResult::SessionTaskLimit);
    CHECK(queue.enqueue("session-b", 20, []() {})
          == SerialTaskQueue::EnqueueResult::SessionByteLimit);

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
}

TEST_CASE(serial_terminal_task_replaces_pending_work) {
    SerialTaskQueue queue(1, smallLimits());
    std::atomic<bool> release{false};
    std::atomic<bool> firstStarted{false};
    std::atomic<bool> staleRan{false};
    std::atomic<bool> terminalRan{false};

    CHECK(queue.enqueue("session-a", 1, [&]() {
        firstStarted.store(true);
        waitUntil([&]() { return release.load(); });
    }) == SerialTaskQueue::EnqueueResult::Accepted);
    waitUntil([&]() { return firstStarted.load(); });

    CHECK(queue.enqueue("session-a", 1, [&]() {
        staleRan.store(true);
    }) == SerialTaskQueue::EnqueueResult::Accepted);
    CHECK(queue.enqueueTerminal("session-a", [&]() {
        terminalRan.store(true);
    }) == SerialTaskQueue::EnqueueResult::Accepted);

    release.store(true);
    waitUntil([&]() { return terminalRan.load(); });
    CHECK(!staleRan.load());
}

TEST_CASE(serial_queue_rejects_work_after_stop) {
    auto limits = smallLimits();
    SerialTaskQueue queue(1, limits);
    queue.stop();

    CHECK(queue.enqueue("session-a", 1, []() {})
          == SerialTaskQueue::EnqueueResult::Stopped);
}

