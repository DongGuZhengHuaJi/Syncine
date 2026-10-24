//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_LOGICSYSTEM_H
#define SERVER_LOGICSYSTEM_H

#include "CloseReason.h"
#include "SerialTaskQueue.h"
#include "ServerOptions.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

class Room;
class Session;

class LogicSystem {
public:
    enum class EnqueueResult {
        Accepted,
        QueueFull,
        Stopped,
    };

    LogicSystem(InboundQueueLimits limits, std::size_t workerCount);
    ~LogicSystem();

    LogicSystem(const LogicSystem &) = delete;
    LogicSystem &operator=(const LogicSystem &) = delete;

    // 同一 Session 的消息按入队顺序串行执行，不同 Session 可并行。
    EnqueueResult enqueueMessage(std::shared_ptr<Session> session,
                                 std::uint64_t generation,
                                 std::string message);

    // 终止任务会丢弃该 Session 尚未开始的消息，但仍排在正在执行的任务后。
    void enqueueDisconnect(std::shared_ptr<Session> session, CloseReason reason);

    void stop();

private:
    // 异常兜底包装：见 .cpp 里的说明
    void handleMessage(std::shared_ptr<Session> session, const std::string &message);
    void dispatchMessage(const std::shared_ptr<Session> &session, const std::string &message);
    void handleDisconnect(const std::shared_ptr<Session> &session,
                          CloseReason reason);

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

    SerialTaskQueue m_serialQueue;

    std::unordered_map<std::string, std::shared_ptr<Room>> m_rooms; // roomId → 房间
    std::unordered_map<std::shared_ptr<Session>, std::string> m_sessionRooms; // 会话 → 所在房间
    std::mutex m_roomsMutex; // 保护房间表（多个工作线程并发处理消息）
};

#endif //SERVER_LOGICSYSTEM_H
