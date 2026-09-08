//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_LOGICSYSTEM_H
#define SERVER_LOGICSYSTEM_H

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class Room;
class Session;

class LogicSystem {
public:
    static LogicSystem &getInstance() {
        static LogicSystem instance;
        return instance;
    }

    // Session 收到消息后调用,投递到逻辑线程
    void enqueueMessage(std::shared_ptr<Session> session, const std::string &message);

    // 连接断开时调用,让成员退出所在房间
    void handleDisconnect(std::shared_ptr<Session> session);

    // 停止所有工作线程(幂等)
    void stop();

private:
    LogicSystem();
    ~LogicSystem();

    void handleMessage(std::shared_ptr<Session> session, const std::string &message);

    void replyError(const std::shared_ptr<Session> &session,
                    const std::string &code, const std::string &message);
    // 要求调用方持有 m_roomsMutex
    bool removeMemberFromRoomLocked(const std::shared_ptr<Session> &session);

    std::queue<std::pair<std::shared_ptr<Session>, std::string>> m_messageQueue;
    std::vector<std::thread> m_workerThreads;

    std::mutex m_mutex; // 保护消息队列
    std::condition_variable m_cv;
    bool m_stop = false;

    std::unordered_map<std::string, std::shared_ptr<Room>> m_rooms; // roomId → 房间
    std::unordered_map<std::shared_ptr<Session>, std::string> m_sessionRooms; // 会话 → 所在房间
    std::mutex m_roomsMutex; // 保护房间表(多个工作线程并发处理消息)
};

#endif //SERVER_LOGICSYSTEM_H
