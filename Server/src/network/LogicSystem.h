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

#include <nlohmann/json.hpp>

class Room;
class Session;

class LogicSystem {
public:
    static LogicSystem &getInstance() {
        static LogicSystem instance;
        return instance;
    }

    // Session 收到消息后调用，投递到逻辑线程
    void enqueueMessage(std::shared_ptr<Session> session, const std::string &message);

    // 连接断开时调用，让成员退出所在房间
    void handleDisconnect(std::shared_ptr<Session> session);

    // 停止所有工作线程（幂等）
    void stop();

private:
    LogicSystem();
    ~LogicSystem();

    // 异常兜底包装：见 .cpp 里的说明
    void handleMessage(std::shared_ptr<Session> session, const std::string &message);
    void dispatchMessage(const std::shared_ptr<Session> &session, const std::string &message);

    void replyError(const std::shared_ptr<Session> &session,
                    const std::string &code, const std::string &message);
    // 当前条目变化时广播一条 playlist_switched，各端据此重新解析该条目的来源
    void broadcastPlaylistSwitched(const std::shared_ptr<Room> &room,
                                   const std::string &itemId);
    // 播放列表消息的统一下发口。共享模式下只发给房主：
    // 条目全是房主本机的文件，观众拿到也没有意义
    void sendPlaylistMessage(const std::shared_ptr<Room> &room, const nlohmann::json &message);
    // 同步模式下把各条目的匹配状态（未匹配/已匹配/不同步）推给所有人
    void broadcastPlaylistStatus(const std::shared_ptr<Room> &room);
    // 要求调用方持有 m_roomsMutex
    bool removeMemberFromRoomLocked(const std::shared_ptr<Session> &session);
    // 按稳定 clientId 移除成员并处理广播/解散。要求持有 m_roomsMutex。
    bool removeMemberByClientIdLocked(const std::shared_ptr<Room> &room,
                                      const std::string &roomId,
                                      const std::string &clientId);
    // 过期离线成员回收（独立线程每秒扫一次）
    void expireOfflineMembers();

    std::queue<std::pair<std::shared_ptr<Session>, std::string>> m_messageQueue;
    std::vector<std::thread> m_workerThreads;

    // 离线成员回收线程
    std::thread m_reaperThread;
    std::condition_variable m_reaperCv;
    std::mutex m_reaperMutex;
    bool m_reaperStop = false;

    std::mutex m_mutex; // 保护消息队列
    std::condition_variable m_cv;
    bool m_stop = false;

    std::unordered_map<std::string, std::shared_ptr<Room>> m_rooms; // roomId → 房间
    std::unordered_map<std::shared_ptr<Session>, std::string> m_sessionRooms; // 会话 → 所在房间
    std::mutex m_roomsMutex; // 保护房间表（多个工作线程并发处理消息）
};

#endif //SERVER_LOGICSYSTEM_H
