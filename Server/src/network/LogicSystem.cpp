//
// Created by donggu on 2026/9/8.
//

#include "LogicSystem.h"
#include "Logger.h"
#include "Room.h"
#include "Session.h"

#include <algorithm>

LogicSystem::LogicSystem() {
    const int numThreads = std::max(1u, std::thread::hardware_concurrency());
    for (int i = 0; i < numThreads; ++i) {
        m_workerThreads.emplace_back([this]() {
            while (true) {
                std::shared_ptr<Session> session;
                std::string message;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this]() {
                        return m_stop || !m_messageQueue.empty();
                    });
                    if (m_stop && m_messageQueue.empty())
                        return;

                    auto item = std::move(m_messageQueue.front());
                    m_messageQueue.pop();
                    session = std::move(item.first);
                    message = std::move(item.second);
                }

                handleMessage(session, message);
            }
        });
    }
}

LogicSystem::~LogicSystem() {
    stop();
}

void LogicSystem::enqueueMessage(std::shared_ptr<Session> session,
                                 const std::string &message) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_messageQueue.push({std::move(session), message});
    m_cv.notify_one();
}

void LogicSystem::handleDisconnect(std::shared_ptr<Session> session) {
    std::lock_guard<std::mutex> lock(m_roomsMutex);
    removeMemberFromRoomLocked(session);
}

void LogicSystem::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop)
            return;
        m_stop = true;
    }
    m_cv.notify_all();
    for (std::thread &thread : m_workerThreads)
        thread.join();
}


// ============================
// 消息处理(协议见 docs/protocol.md)
// ============================

void LogicSystem::handleMessage(std::shared_ptr<Session> session,
                                const std::string &text) {
    json message;
    try {
        message = json::parse(text);
    } catch (const std::exception &) {
        replyError(session, "bad_message", "消息格式错误");
        return;
    }

    const std::string type = message.value("type", "");

    std::lock_guard<std::mutex> lock(m_roomsMutex);

    if (type == "create_room") {
        const std::string newRoomId = message.value("roomId", "");
        if (newRoomId.empty()) {
            replyError(session, "bad_message", "缺少房间号");
            return;
        }
        if (m_rooms.count(newRoomId)) {
            replyError(session, "room_exists", "房间号冲突,请重试");
            return;
        }

        const std::string mode = message.value("mode", "local");
        if (mode != "local" && mode != "share" && mode != "url") {
            replyError(session, "bad_message", "未知的房间模式");
            return;
        }

        // 已在别的房间:先退出
        removeMemberFromRoomLocked(session);

        auto room = std::make_shared<Room>(
            message.value("roomName", ""),
            message.value("password", ""),
            mode);
        room->addMember(session, message.value("nickname", ""), true);

        m_rooms[newRoomId] = room;
        m_sessionRooms[session] = newRoomId;

        json reply;
        reply["type"] = "room_created";
        reply["roomId"] = newRoomId;
        reply["roomName"] = room->roomName();
        reply["mode"] = room->mode();
        reply["members"] = room->membersArray();
        reply["state"] = room->stateJson();
        reply["videoMismatched"] = room->videoMismatched();
        reply["shortestDuration"] = room->shortestDuration();
        session->send(reply.dump());
        Logger::info("房间创建: " + newRoomId);
        return;
    }

    if (type == "join_room") {
        const std::string targetRoomId = message.value("roomId", "");
        auto it = m_rooms.find(targetRoomId);
        if (it == m_rooms.end()) {
            replyError(session, "room_not_found", "房间不存在");
            return;
        }
        // 拷贝 shared_ptr,后续 removeMemberFromRoomLocked 改动 map 也不影响
        const auto room = it->second;

        if (room->hasPassword()
            && !room->checkPassword(message.value("password", ""))) {
            replyError(session, "wrong_password", "密码错误");
            return;
        }

        const std::string nickname = message.value("nickname", "");
        if (room->hasNickname(nickname)) {
            replyError(session, "nickname_taken", "昵称已被占用");
            return;
        }

        // 已在别的房间:先退出,再重新查找
        removeMemberFromRoomLocked(session);
        it = m_rooms.find(targetRoomId);
        if (it == m_rooms.end()) {
            replyError(session, "room_not_found", "房间不存在");
            return;
        }
        const auto targetRoom = it->second;
        targetRoom->addMember(session, nickname, false);
        m_sessionRooms[session] = targetRoomId;

        json reply;
        reply["type"] = "room_joined";
        reply["roomId"] = targetRoomId;
        reply["roomName"] = targetRoom->roomName();
        reply["mode"] = targetRoom->mode();
        reply["members"] = targetRoom->membersArray();
        reply["state"] = targetRoom->stateJson();
        reply["videoMismatched"] = targetRoom->videoMismatched();
        reply["shortestDuration"] = targetRoom->shortestDuration();
        session->send(reply.dump());

        json notice;
        notice["type"] = "member_joined";
        notice["clientId"] = session->id();
        notice["nickname"] = nickname;
        notice["isHost"] = false;
        notice["loaded"] = false;
        notice["duration"] = 0;
        targetRoom->broadcast(notice, session);
        Logger::info("成员加入 " + targetRoomId + ": " + nickname);
        return;
    }

    if (type == "leave_room") {
        removeMemberFromRoomLocked(session);
        return;
    }

    if (type == "set_room_mode") {
        auto roomIt = m_sessionRooms.find(session);
        if (roomIt == m_sessionRooms.end()) {
            replyError(session, "not_in_room", "你不在房间里");
            return;
        }
        auto it = m_rooms.find(roomIt->second);
        if (it == m_rooms.end()) {
            replyError(session, "not_in_room", "你不在房间里");
            return;
        }
        const auto room = it->second;

        if (!room->isHost(session)) {
            replyError(session, "not_host", "只有房主可以更改房间模式");
            return;
        }

        const std::string mode = message.value("mode", "");
        if (mode != "local" && mode != "share" && mode != "url") {
            replyError(session, "bad_message", "未知的房间模式");
            return;
        }

        room->setMode(mode);

        // 广播给所有成员(含房主自己),客户端以服务端为准
        json notice;
        notice["type"] = "room_mode_changed";
        notice["mode"] = mode;
        room->broadcast(notice);
        Logger::info("房间 " + roomIt->second + " 模式切换: " + mode);
        return;
    }

    if (type == "video_status") {
        auto roomIt = m_sessionRooms.find(session);
        if (roomIt == m_sessionRooms.end()) {
            replyError(session, "not_in_room", "你不在房间里");
            return;
        }
        auto it = m_rooms.find(roomIt->second);
        if (it == m_rooms.end()) {
            replyError(session, "not_in_room", "你不在房间里");
            return;
        }
        const auto room = it->second;

        const bool loaded = message.value("loaded", false);
        const std::string hash = message.value("hash", "");
        const long long duration = message.value("duration", 0LL);

        room->setMemberVideo(session, loaded, hash, duration);

        // 中继给其他人(不含哈希,哈希只留在服务端做比对)
        json relay;
        relay["type"] = "video_status";
        relay["clientId"] = session->id();
        relay["loaded"] = loaded;
        relay["duration"] = duration;
        room->broadcast(relay, session);

        // 不一致状态变化时通知所有人
        if (room->updateMismatch()) {
            json notice;
            notice["type"] = "video_mismatch";
            notice["mismatched"] = room->videoMismatched();
            notice["shortestDuration"] = room->shortestDuration();
            room->broadcast(notice);
        }
        return;
    }

    if (type == "webrtc_offer" || type == "webrtc_answer" || type == "webrtc_ice") {
        auto roomIt = m_sessionRooms.find(session);
        if (roomIt == m_sessionRooms.end()) {
            replyError(session, "not_in_room", "你不在房间里");
            return;
        }
        auto it = m_rooms.find(roomIt->second);
        if (it == m_rooms.end()) {
            replyError(session, "not_in_room", "你不在房间里");
            return;
        }
        const auto room = it->second;

        // 定向转发:目标必须与发送者在同一个房间
        const std::string targetId = message.value("to", "");
        const auto target = room->findSession(targetId);
        if (!target) {
            replyError(session, "member_not_found", "目标成员不存在");
            return;
        }

        json relay = message;
        relay["from"] = session->id();
        target->send(relay.dump());
        return;
    }

    if (type == "chat" || type == "playback") {
        auto roomIt = m_sessionRooms.find(session);
        if (roomIt == m_sessionRooms.end()) {
            replyError(session, "not_in_room", "你不在房间里");
            return;
        }
        auto it = m_rooms.find(roomIt->second);
        if (it == m_rooms.end()) {
            replyError(session, "not_in_room", "你不在房间里");
            return;
        }
        const auto room = it->second;
        const std::string nickname = room->nicknameOf(session);

        if (type == "chat") {
            json relay;
            relay["type"] = "chat";
            relay["from"] = nickname;
            relay["text"] = message.value("text", "");
            room->broadcast(relay, session);
        } else {
            const std::string action = message.value("action", "");
            const long long position = message.value("position", 0LL);

            // 记录房间最后播放状态,新成员加入时据此对齐
            room->updatePlayback(action, position);

            json relay;
            relay["type"] = "playback";
            relay["from"] = nickname;
            relay["action"] = action;
            relay["position"] = position;
            room->broadcast(relay, session);
        }
        return;
    }

    replyError(session, "bad_message", "未知的消息类型");
}


// ============================
// 辅助
// ============================

void LogicSystem::replyError(const std::shared_ptr<Session> &session,
                             const std::string &code,
                             const std::string &message) {
    json reply;
    reply["type"] = "error";
    reply["code"] = code;
    reply["message"] = message;
    session->send(reply.dump());
}

bool LogicSystem::removeMemberFromRoomLocked(const std::shared_ptr<Session> &session) {
    auto roomIt = m_sessionRooms.find(session);
    if (roomIt == m_sessionRooms.end())
        return false;

    const std::string roomId = roomIt->second;
    m_sessionRooms.erase(roomIt);

    auto it = m_rooms.find(roomId);
    if (it == m_rooms.end())
        return false;

    const auto room = it->second; // 拷贝 shared_ptr,erase 后对象仍存活
    const bool wasHost = room->isHost(session);
    const std::string nickname = room->nicknameOf(session);
    room->removeMember(session);

    if (room->isEmpty() || wasHost) {
        // 房主离开:解散房间,通知剩余成员
        json notice;
        notice["type"] = "room_left";
        for (const auto &memberSession : room->sessions()) {
            memberSession->send(notice.dump());
            m_sessionRooms.erase(memberSession);
        }
        m_rooms.erase(it);
        Logger::info("房间解散: " + roomId);
    } else {
        json notice;
        notice["type"] = "member_left";
        notice["clientId"] = session->id();
        notice["nickname"] = nickname;
        room->broadcast(notice); // 离开者已移除,自动只发给剩余成员
        Logger::info("成员离开 " + roomId + ": " + nickname);

        // 离开的成员可能带走了不一致的视频,重新计算
        if (room->updateMismatch()) {
            json mismatch;
            mismatch["type"] = "video_mismatch";
            mismatch["mismatched"] = room->videoMismatched();
            mismatch["shortestDuration"] = room->shortestDuration();
            room->broadcast(mismatch);
        }
    }
    return true;
}
