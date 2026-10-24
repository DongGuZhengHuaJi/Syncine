#include "SerialTaskQueue.h"

#include <algorithm>
#include <utility>

SerialTaskQueue::SerialTaskQueue(std::size_t workerCount, Limits limits)
    : m_limits(limits) {
    workerCount = std::max<std::size_t>(1, workerCount);
    m_workers.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i)
        m_workers.emplace_back([this]() { runWorker(); });
}

SerialTaskQueue::~SerialTaskQueue() {
    stop();
}

SerialTaskQueue::EnqueueResult SerialTaskQueue::enqueue(
    std::string key, std::size_t bytes, std::function<void()> task) {
    std::lock_guard lock(m_mutex);
    if (m_stop)
        return EnqueueResult::Stopped;

    auto queueIt = m_queues.find(key);
    const QueueState emptyQueue;
    const QueueState &queue =
        queueIt == m_queues.end() ? emptyQueue : queueIt->second;
    if (queue.entries.size() >= m_limits.maxSessionTasks)
        return EnqueueResult::SessionTaskLimit;
    if (queue.bytes + bytes > m_limits.maxSessionBytes)
        return EnqueueResult::SessionByteLimit;
    if (m_queuedTasks >= m_limits.maxGlobalTasks)
        return EnqueueResult::GlobalTaskLimit;
    if (m_queuedBytes + bytes > m_limits.maxGlobalBytes)
        return EnqueueResult::GlobalByteLimit;

    if (queueIt == m_queues.end())
        queueIt = m_queues.emplace(key, QueueState{}).first;

    QueueState &destination = queueIt->second;
    const bool shouldSchedule = !destination.running;
    destination.entries.push_back({bytes, std::move(task)});
    destination.bytes += bytes;
    destination.running = true;
    ++m_queuedTasks;
    m_queuedBytes += bytes;

    if (shouldSchedule)
        m_ready.push_back(std::move(key));
    m_cv.notify_one();
    return EnqueueResult::Accepted;
}

SerialTaskQueue::EnqueueResult SerialTaskQueue::enqueueTerminal(
    std::string key, std::function<void()> task) {
    std::lock_guard lock(m_mutex);
    if (m_stop)
        return EnqueueResult::Stopped;

    auto &queue = m_queues[key];
    const bool shouldSchedule = !queue.running;
    subtractQueuedLocked(queue);
    queue.entries.clear();

    queue.entries.push_back({0, std::move(task)});
    queue.running = true;
    ++m_queuedTasks;

    if (shouldSchedule)
        m_ready.push_back(std::move(key));
    m_cv.notify_one();
    return EnqueueResult::Accepted;
}

void SerialTaskQueue::stop() {
    {
        std::lock_guard lock(m_mutex);
        if (m_stop)
            return;
        m_stop = true;
        m_ready.clear();
        m_queues.clear();
        m_queuedTasks = 0;
        m_queuedBytes = 0;
    }
    m_cv.notify_all();

    for (std::thread &worker : m_workers) {
        if (worker.joinable())
            worker.join();
    }
}

void SerialTaskQueue::subtractQueuedLocked(const QueueState &queue) {
    m_queuedTasks -= queue.entries.size();
    m_queuedBytes -= queue.bytes;
}

void SerialTaskQueue::runWorker() {
    while (true) {
        std::function<void()> task;
        std::string key;

        {
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this]() {
                return m_stop || !m_ready.empty();
            });
            if (m_stop)
                return;

            key = std::move(m_ready.front());
            m_ready.pop_front();

            auto queueIt = m_queues.find(key);
            if (queueIt == m_queues.end() || queueIt->second.entries.empty())
                continue;

            Entry entry = std::move(queueIt->second.entries.front());
            queueIt->second.entries.pop_front();
            queueIt->second.bytes -= entry.bytes;
            --m_queuedTasks;
            m_queuedBytes -= entry.bytes;
            task = std::move(entry.task);
        }

        if (task)
            task();

        {
            std::lock_guard lock(m_mutex);
            auto queueIt = m_queues.find(key);
            if (queueIt == m_queues.end())
                continue;

            if (queueIt->second.entries.empty()) {
                m_queues.erase(queueIt);
            } else {
                m_ready.push_back(key);
                m_cv.notify_one();
            }
        }
    }
}

