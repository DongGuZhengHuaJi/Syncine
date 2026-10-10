//
// Created by donggu on 2026/9/8.
//

#include "LogicSystem.h"
#include "Log.h"
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
        reply["state"] = room->stateJson(); // 建房的人就是房主,列表给全
        reply["videoMismatched"] = room->videoMismatched();
        reply["shortestDuration"] = room->shortestDuration();
        session->send(reply.dump());
        LOG_INFO("Logic") << "房间创建: " << newRoomId;
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
        // 共享模式下播放列表是房主自己的文件清单,观众不参与,给空列表
        reply["state"] = targetRoom->stateJson(targetRoom->mode() != "share"
                                               || targetRoom->isHost(session));
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

        // 新成员一条文件都还没有 —— 同步模式下所有条目的"已匹配"都要退回"未匹配",
        // 得让所有人重算一次
        broadcastPlaylistStatus(targetRoom);

        LOG_INFO("Logic") << "成员加入 " << targetRoomId << ": " << nickname;
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

        // 播放列表是**按模式各存一份**的 —— 换模式等于换了一份列表,
        // 得把新的那份推下去(否则界面上还显示着上一个模式的条目)
        json changed;
        changed["type"] = "playlist_changed";
        changed["items"] = room->playlistJson();
        changed["currentIndex"] = room->currentIndex();
        sendPlaylistMessage(room, changed); // 共享模式只发给房主,其余模式广播

        if (mode == "share") {
            // 共享模式下观众不参与列表:他们必须拿到"空列表 + 无当前条目",
            // 否则界面上会一直留着上一个模式的旧列表
            json cleared;
            cleared["type"] = "playlist_changed";
            cleared["items"] = json::array();
            cleared["currentIndex"] = -1;

            // 顺带把观众的播放器也卸掉 —— 他们看不到列表,当前条目自然也没了
            json unloaded;
            unloaded["type"] = "playlist_switched";
            unloaded["itemId"] = "";
            unloaded["currentIndex"] = -1;

            if (const auto host = room->hostSession()) {
                room->broadcast(cleared, host);
                room->broadcast(unloaded, host);
            } else {
                room->broadcast(cleared);
                room->broadcast(unloaded);
            }
        }

        broadcastPlaylistSwitched(room, room->currentItemId());
        broadcastPlaylistStatus(room);

        LOG_INFO("Logic") << "房间 " << roomIt->second << " 模式切换: " << mode;
        return;
    }

    // 同步模式下成员上报"这一条我这台机器上有没有文件"
    if (type == "playlist_status") {
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

        // 只有同步模式有"匹配"这回事。别的模式收到就当没看见 ——
        // 可能是刚切完模式、客户端还没来得及停发,不值得报错打扰用户
        if (room->mode() == "local") {
            room->setMemberFile(session->id(),
                                message.value("itemId", ""),
                                message.value("hasFile", false),
                                message.value("hash", ""));
            broadcastPlaylistStatus(room);
        }
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

    // ── 播放列表 ──────────────────────────────────────────
    //
    // 列表是房间共享的一份("我们接下来要看这几部"),条目的**本地文件路径不在里面** ——
    // 各人自己把 itemId 映射到自己机器上的文件。所以服务端只做三件事:
    // 存条目、维护"当前是第几条"、把变化广播出去。
    if (type == "playlist_add" || type == "playlist_remove" || type == "playlist_switch") {
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
        const bool isHostSession = room->isHost(session);

        if (type == "playlist_add") {
            // 共享模式下只有房主能加:别人的本地文件房主没有,加进来谁也播不了
            if (room->mode() == "share" && !isHostSession) {
                replyError(session, "not_host", "共享模式下只有房主能添加视频");
                return;
            }

            PlaylistEntry entry;
            entry.itemId = message.value("itemId", "");
            entry.title = message.value("title", "");
            entry.url = message.value("url", "");
            entry.duration = message.value("duration", 0LL);
            entry.addedBy = session->id();

            if (entry.itemId.empty() || entry.title.empty()) {
                replyError(session, "bad_request", "播放列表条目缺少 itemId 或标题");
                return;
            }

            // itemId 由客户端生成(UUID),服务端只保证不重复
            for (const PlaylistEntry &existing : room->playlist()) {
                if (existing.itemId == entry.itemId) {
                    replyError(session, "duplicate_item", "这个条目已经在播放列表里了");
                    return;
                }
            }

            const bool becameCurrent = room->addEntry(entry);

            // 广播给**所有人**(包括添加者):他也要拿服务端的权威列表,不做本地乐观插入
            json changed;
            changed["type"] = "playlist_changed";
            changed["items"] = room->playlistJson();
            changed["currentIndex"] = room->currentIndex();
            sendPlaylistMessage(room, changed);

            // 列表本来是空的 → 这一条成了当前条目,各端要据此去加载
            if (becameCurrent)
                broadcastPlaylistSwitched(room, room->currentItemId());

            LOG_INFO("Logic") << "播放列表添加: " << roomIt->second << " ← " << entry.title;
            return;
        }

        // 删除和切集都只有房主能做 —— 和切模式一样,房间级的动作
        if (!isHostSession) {
            replyError(session, "not_host", "只有房主能整理播放列表");
            return;
        }

        const std::string itemId = message.value("itemId", "");
        const std::string beforeId = room->currentItemId();

        if (type == "playlist_remove") {
            if (!room->removeEntry(itemId)) {
                replyError(session, "item_not_found", "播放列表里没有这个条目");
                return;
            }

            json changed;
            changed["type"] = "playlist_changed";
            changed["items"] = room->playlistJson();
            changed["currentIndex"] = room->currentIndex();
            sendPlaylistMessage(room, changed);

            LOG_INFO("Logic") << "播放列表移除: " << roomIt->second << " ← " << itemId;
        } else {
            if (!room->switchTo(itemId)) {
                replyError(session, "item_not_found", "播放列表里没有这个条目");
                return;
            }
            LOG_INFO("Logic") << "播放列表切集: " << roomIt->second << " → " << itemId;
        }

        // 当前条目变了才广播切换(删掉的正好是当前那条时也会走到这里)
        if (room->currentItemId() != beforeId)
            broadcastPlaylistSwitched(room, room->currentItemId());
        return;
    }

    if (type == "chat" || type == "playback" || type == "playback_position") {
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
        } else if (type == "playback_position") {
            // 房主周期性的位置广播:透传 + 盖发送者,不更新房间状态
            json relay = message;
            relay["from"] = nickname;
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

void LogicSystem::broadcastPlaylistSwitched(const std::shared_ptr<Room> &room,
                                            const std::string &itemId) {
    json switched;
    switched["type"] = "playlist_switched";
    switched["itemId"] = itemId;
    switched["currentIndex"] = room->currentIndex();
    sendPlaylistMessage(room, switched);
}

void LogicSystem::sendPlaylistMessage(const std::shared_ptr<Room> &room,
                                      const json &message) {
    // 共享模式下播放列表只有房主有:条目是他本机的文件,别人既看不到也用不上。
    // 同步/网链模式则是房间共享的一份,发给所有人。
    if (room->mode() == "share") {
        if (const auto host = room->hostSession())
            host->send(message.dump());
        return;
    }
    room->broadcast(message);
}

void LogicSystem::broadcastPlaylistStatus(const std::shared_ptr<Room> &room) {
    // "匹配"只在同步模式有意义:共享模式只有房主有文件,网链模式大家读同一个 URL
    if (room->mode() != "local")
        return;

    json statuses = json::array();
    for (const PlaylistEntry &entry : room->playlist()) {
        statuses.push_back({
            {"itemId", entry.itemId},
            {"status", room->itemStatus(entry.itemId)},
        });
    }

    json message;
    message["type"] = "playlist_status";
    message["items"] = statuses;
    sendPlaylistMessage(room, message);
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

    // 走掉的人对播放列表的上报也要一起清掉 —— 否则"全员都加载了"会把
    // 已经离开的人也算进去,状态永远是错的
    room->dropMemberFiles(session->id());

    if (room->isEmpty() || wasHost) {
        // 房主离开:解散房间,通知剩余成员
        json notice;
        notice["type"] = "room_left";
        for (const auto &memberSession : room->sessions()) {
            memberSession->send(notice.dump());
            m_sessionRooms.erase(memberSession);
        }
        m_rooms.erase(it);
        LOG_INFO("Logic") << "房间解散: " << roomId;
    } else {
        json notice;
        notice["type"] = "member_left";
        notice["clientId"] = session->id();
        notice["nickname"] = nickname;
        room->broadcast(notice); // 离开者已移除,自动只发给剩余成员

        // 少了一个人,匹配状态要重算(可能从"未匹配"变成"已匹配")
        broadcastPlaylistStatus(room);

        LOG_INFO("Logic") << "成员离开 " << roomId << ": " << nickname;

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
