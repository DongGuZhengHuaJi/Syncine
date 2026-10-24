#ifndef SERVER_SERIALTASKQUEUE_H
#define SERVER_SERIALTASKQUEUE_H

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class SerialTaskQueue {
public:
    enum class EnqueueResult {
        Accepted,
        SessionTaskLimit,
        SessionByteLimit,
        GlobalTaskLimit,
        GlobalByteLimit,
        Stopped,
    };

    struct Limits {
        std::size_t maxSessionTasks = 64;
        std::size_t maxSessionBytes = 1024 * 1024;
        std::size_t maxGlobalTasks = 4096;
        std::size_t maxGlobalBytes = 16 * 1024 * 1024;
    };

    SerialTaskQueue(std::size_t workerCount, Limits limits);
    ~SerialTaskQueue();

    SerialTaskQueue(const SerialTaskQueue &) = delete;
    SerialTaskQueue &operator=(const SerialTaskQueue &) = delete;

    EnqueueResult enqueue(std::string key, std::size_t bytes,
                          std::function<void()> task);
    // A terminal task discards work that has not started yet and is never
    // rejected because of queue capacity.
    EnqueueResult enqueueTerminal(std::string key, std::function<void()> task);
    void stop();

private:
    struct Entry {
        std::size_t bytes = 0;
        std::function<void()> task;
    };

    struct QueueState {
        std::deque<Entry> entries;
        std::size_t bytes = 0;
        bool running = false;
    };

    void subtractQueuedLocked(const QueueState &queue);
    void runWorker();

    const Limits m_limits;
    std::vector<std::thread> m_workers;
    std::unordered_map<std::string, QueueState> m_queues;
    std::deque<std::string> m_ready;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    std::size_t m_queuedTasks = 0;
    std::size_t m_queuedBytes = 0;
};

#endif // SERVER_SERIALTASKQUEUE_H

