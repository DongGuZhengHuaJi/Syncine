//
// Created by donggu on 2026/9/8.
//

#include "LogicSystem.h"
#include "Config.h"
#include "Log.h"
#include "Room.h"
#include "Session.h"
#include "TurnCredentials.h"

#include <algorithm>
#include <chrono>

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

    // 离线成员回收线程：每秒扫一次，超过宽限期的成员才真正移除。
    // 放在独立线程而不是 worker 里，是因为即使房间没有新消息，
    // 过期成员也必须被清理，否则房间/成员表会一直占着。
    m_reaperThread = std::thread([this]() {
        std::unique_lock<std::mutex> lock(m_reaperMutex);
        while (!m_reaperStop) {
            m_reaperCv.wait_for(lock, std::chrono::seconds(1));
            if (m_reaperStop)
                return;
            lock.unlock();
            expireOfflineMembers();
            lock.lock();
        }
    });
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
    LOG_DEBUG("Logic") << "连接断开: clientId=" << session->id();

    std::lock_guard<std::mutex> lock(m_roomsMutex);
    auto roomIt = m_sessionRooms.find(session);
    if (roomIt == m_sessionRooms.end()) {
        // 可能是已经被 resume 替换掉的旧连接，或从未进过房间
        return;
    }

    const std::string roomId = roomIt->second;
    auto it = m_rooms.find(roomId);
    if (it == m_rooms.end()) {
        m_sessionRooms.erase(roomIt);
        return;
    }

    const auto room = it->second;
    // 保留成员记录和稳定 clientId，等客户端用 resume token 回来。
    // 这期间不广播 member_left，其他人不会看到"退出又加入"。
    const std::string clientId = room->clientIdOf(session);
    const std::string nickname = room->nicknameOf(session);
    if (room->markMemberOffline(session, std::chrono::steady_clock::now())) {
        // 只广播状态变化，不广播 member_left：成员表不变，UI 显示"重连中"
        json offline;
        offline["type"] = "member_offline";
        offline["clientId"] = clientId;
        offline["nickname"] = nickname;
        room->broadcast(offline);

        m_sessionRooms.erase(roomIt);
        LOG_INFO("Logic") << "成员进入离线等待: roomId=" << roomId
                          << " clientId=" << clientId
                          << " grace=" << Config::MEMBER_OFFLINE_GRACE_SECONDS << "s";
        return;
    }

    // 不是成员（例如旧连接被替换后才关闭）：只清映射
    m_sessionRooms.erase(roomIt);
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

    // 回收线程也必须在清空房间表之前停掉
    {
        std::lock_guard<std::mutex> lock(m_reaperMutex);
        m_reaperStop = true;
    }
    m_reaperCv.notify_all();
    if (m_reaperThread.joinable())
        m_reaperThread.join();

    // 线程已 join，此时清空房间表是安全的。这一步必须在 io_context 还活着
    // 的时候做：Session 的析构会去碰 socket/executor，而 LogicSystem 是
    // 函数内静态对象，它的析构发生在 main 返回之后 —— 那时 io_context 早已
    // 销毁了。stop() 由 WebSocketServer::stop() 调用，时机正好。
    {
        std::lock_guard<std::mutex> lock(m_roomsMutex);
        m_sessionRooms.clear();
        m_rooms.clear();
    }
}


// ============================
// 消息处理（协议见 docs/protocol.md）
// ============================

// 异常兜底。
//
// 这个 try 不是可有可无的防御性代码：工作线程的入口是 std::thread 的
// lambda，异常一旦穿透出去就是 std::terminate —— 整个服务端进程直接消失。
// nlohmann 的 value() 在**类型不符**时抛 type_error（键存在但不是要的类型），
// 而不只是 JSON 语法错误时抛。实测两条消息即可打死服务端：
//     [1,2,3]                          根不是 object      → type_error.306
//     {"type":"chat","text":42}        text 不是 string   → type_error.302
// 后者连房间都不用进。协议里每个字段都取自外部输入，所以这里必须兜住。
void LogicSystem::handleMessage(std::shared_ptr<Session> session,
                                const std::string &text) {
    try {
        dispatchMessage(session, text);
    } catch (const std::exception &error) {
        LOG_WARN("Logic") << "处理消息时抛出异常 clientId=" << session->id()
                          << ": " << error.what();
        replyError(session, "bad_message", "消息格式错误");
    } catch (...) {
        LOG_WARN("Logic") << "处理消息时抛出未知异常 clientId=" << session->id();
        replyError(session, "bad_message", "消息格式错误");
    }
}

void LogicSystem::dispatchMessage(const std::shared_ptr<Session> &session,
                                  const std::string &text) {
    json message;
    try {
        message = json::parse(text);
    } catch (const std::exception &error) {
        LOG_WARN("Logic") << "消息解析失败 clientId=" << session->id()
                          << ": " << error.what();
        replyError(session, "bad_message", "消息格式错误");
        return;
    }

    const std::string type = message.value("type", "");
    LOG_DEBUG("Logic") << "处理消息: type=" << type << " clientId=" << session->id();

    std::lock_guard<std::mutex> lock(m_roomsMutex);

    if (type == "create_room") {
        const std::string newRoomId = message.value("roomId", "");
        if (newRoomId.empty()) {
            replyError(session, "bad_message", "缺少房间号");
            return;
        }
        if (m_rooms.count(newRoomId)) {
            replyError(session, "room_exists", "房间号冲突，请重试");
            return;
        }
        // 房间表无上限时，一个客户端循环建房即可吃光内存
        if (m_rooms.size() >= Config::MAX_ROOMS) {
            replyError(session, "server_full", "服务器房间数已达上限，请稍后再试");
            return;
        }

        const std::string mode = message.value("mode", "local");
        if (mode != "local" && mode != "share" && mode != "url") {
            replyError(session, "bad_message", "未知的房间模式");
            return;
        }

        // 已在其他房间：先退出
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
        // 稳定成员 id：和 welcome 里的连接 id 相同（首次进房），
        // 重连时 resume 会用原 id 覆盖客户端手里的值。
        reply["clientId"] = session->id();
        // 断线重连凭据：只发给本人，其他成员看不到
        reply["resumeToken"] = room->resumeTokenFor(session);
        reply["roomName"] = room->roomName();
        reply["mode"] = room->mode();
        reply["members"] = room->membersArray();
        reply["state"] = room->stateJson(); // 建房者即房主，播放列表给全
        reply["videoMismatched"] = room->videoMismatched();
        reply["shortestDuration"] = room->shortestDuration();
        // ICE 配置（STUN + 带临时凭据的 TURN）随入房消息一并下发：客户端进房后
        // 立即要建 PeerConnection，单独发一条会晚于建连。放顶层而非塞进 state，
        // state 是房间播放状态，这是网络配置。内容由 TurnCredentials 兑换并缓存。
        reply["iceServers"] = TurnCredentials::getInstance().iceServers();
        session->send(reply.dump());
        LOG_INFO("Logic") << "房间创建: " << newRoomId
                          << " mode=" << room->mode()
                          << " nickname=" << message.value("nickname", "");
        return;
    }

    if (type == "join_room") {
        const std::string targetRoomId = message.value("roomId", "");
        auto it = m_rooms.find(targetRoomId);
        if (it == m_rooms.end()) {
            replyError(session, "room_not_found", "房间不存在");
            return;
        }
        // 拷贝 shared_ptr：后续 removeMemberFromRoomLocked 改动 map 不影响此处
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

        // 已在其他房间：先退出，再重新查找
        removeMemberFromRoomLocked(session);
        it = m_rooms.find(targetRoomId);
        if (it == m_rooms.end()) {
            replyError(session, "room_not_found", "房间不存在");
            return;
        }
        const auto targetRoom = it->second;
        // 先判人数再入房：addMember 无上限时，一个客户端循环 join 即可塞满。
        // 离线等待重连的成员也占名额，否则他们回来时会超过上限。
        if (targetRoom->memberCount() >= Config::MAX_MEMBERS_PER_ROOM) {
            replyError(session, "room_full", "房间已满");
            return;
        }
        targetRoom->addMember(session, nickname, false);
        m_sessionRooms[session] = targetRoomId;

        json reply;
        reply["type"] = "room_joined";
        reply["roomId"] = targetRoomId;
        reply["clientId"] = session->id();
        reply["resumeToken"] = targetRoom->resumeTokenFor(session);
        reply["roomName"] = targetRoom->roomName();
        reply["mode"] = targetRoom->mode();
        reply["members"] = targetRoom->membersArray();
        // 共享模式下播放列表是房主自己的文件清单，观众不参与，给空列表
        reply["state"] = targetRoom->stateJson(targetRoom->mode() != "share"
                                               || targetRoom->isHost(session));
        reply["videoMismatched"] = targetRoom->videoMismatched();
        reply["shortestDuration"] = targetRoom->shortestDuration();
        // 同 create_room：ICE 配置跟着入房消息走
        reply["iceServers"] = TurnCredentials::getInstance().iceServers();
        session->send(reply.dump());

        json notice;
        notice["type"] = "member_joined";
        notice["clientId"] = targetRoom->clientIdOf(session);
        notice["nickname"] = nickname;
        notice["isHost"] = false;
        notice["loaded"] = false;
        notice["duration"] = 0;
        targetRoom->broadcast(notice, session);

        // 新成员尚无任何文件，同步模式下所有条目都要从已匹配退回未匹配，
        // 需让所有人重算一次
        broadcastPlaylistStatus(targetRoom);

        LOG_INFO("Logic") << "成员加入 " << targetRoomId << ": " << nickname
                          << " clientId=" << session->id()
                          << " 房间人数=" << targetRoom->sessions().size();
        return;
    }

    if (type == "resume") {
        // 断线重连：用 token 换回原来的成员记录和稳定 clientId。
        // 成功时不广播 member_joined —— 其他成员的成员表没有变化。
        const std::string roomId = message.value("roomId", "");
        const std::string token = message.value("token", "");
        const std::string clientId = message.value("clientId", "");
        if (roomId.empty() || token.empty() || clientId.empty()) {
            replyError(session, "resume_failed", "缺少恢复凭据");
            return;
        }

        auto it = m_rooms.find(roomId);
        if (it == m_rooms.end()) {
            replyError(session, "room_not_found", "房间不存在");
            return;
        }
        const auto room = it->second;

        const auto oldSession = room->resumeMember(clientId, token, session);
        if (oldSession == nullptr) {
            replyError(session, "resume_failed", "会话已过期");
            return;
        }

        // 旧连接可能还半开着（服务端还没发现它断了）。解除映射并异步关闭，
        // 不能在这里同步 close：onClose → handleDisconnect 会再抢 m_roomsMutex。
        if (oldSession != session) {
            m_sessionRooms.erase(oldSession);
            oldSession->stopLater();
        }
        m_sessionRooms[session] = roomId;

        json reply;
        reply["type"] = "room_joined";
        reply["resumed"] = true;
        reply["roomId"] = roomId;
        reply["clientId"] = clientId;
        reply["resumeToken"] = room->resumeTokenFor(session);
        reply["roomName"] = room->roomName();
        reply["mode"] = room->mode();
        reply["members"] = room->membersArray();
        reply["state"] = room->stateJson(room->mode() != "share"
                                         || room->isHost(session));
        reply["videoMismatched"] = room->videoMismatched();
        reply["shortestDuration"] = room->shortestDuration();
        reply["iceServers"] = TurnCredentials::getInstance().iceServers();
        session->send(reply.dump());

        // 告诉其他在线成员他回来了：对端在离线期间发出的 ICE restart 会
        // 收到 member_not_found，收到这条后可以重新尝试。
        json online;
        online["type"] = "member_online";
        online["clientId"] = clientId;
        online["nickname"] = room->nicknameOf(session);
        online["isHost"] = room->isHost(session);
        room->broadcast(online, session);

        LOG_INFO("Logic") << "成员恢复会话 " << roomId
                          << ": clientId=" << clientId
                          << " 在线人数=" << room->onlineCount();
        return;
    }

    if (type == "leave_room") {
        removeMemberFromRoomLocked(session);
        return;
    }

    if (type == "set_room_mode") {
        auto roomIt = m_sessionRooms.find(session);
        if (roomIt == m_sessionRooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
            return;
        }
        auto it = m_rooms.find(roomIt->second);
        if (it == m_rooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
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

        const std::string beforeItem = room->currentItemId();
        room->setMode(mode);
        // 换模式等于换一份播放列表：当前条目变了，旧的成员视频状态全部作废
        if (room->currentItemId() != beforeItem)
            room->clearMemberVideoStates();

        // 广播给所有成员（含房主自己），客户端以服务端为准
        json notice;
        notice["type"] = "room_mode_changed";
        notice["mode"] = mode;
        room->broadcast(notice);

        // 播放列表按模式各存一份，换模式等于换了一份列表，需推送新的那份，
        // 否则界面上仍显示上一个模式的条目
        json changed;
        changed["type"] = "playlist_changed";
        changed["items"] = room->playlistJson();
        changed["currentIndex"] = room->currentIndex();
        sendPlaylistMessage(room, changed); // 共享模式仅发房主，其余模式广播

        if (mode == "share") {
            // 共享模式下观众不参与列表，必须拿到空列表与无当前条目，
            // 否则界面上会一直保留上一个模式的旧列表
            json cleared;
            cleared["type"] = "playlist_changed";
            cleared["items"] = json::array();
            cleared["currentIndex"] = -1;

            // 同时卸载观众的播放器：他们看不到列表，当前条目自然不复存在
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
        // 模式切换后当前条目已经换了一份列表。旧条目的 loaded/时长不能
        // 继续参与一致性判断，立即按新当前条目重算一次。
        broadcastMismatchIfChanged(room);

        LOG_INFO("Logic") << "房间 " << roomIt->second << " 模式切换: " << mode;
        return;
    }

    // 同步模式下成员上报「这一条本机是否有文件」
    if (type == "playlist_status") {
        auto roomIt = m_sessionRooms.find(session);
        if (roomIt == m_sessionRooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
            return;
        }
        auto it = m_rooms.find(roomIt->second);
        if (it == m_rooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
            return;
        }
        const auto room = it->second;

        // 匹配仅同步模式有意义。其他模式收到即忽略：可能是刚切完模式、
        // 客户端尚未停发，不值得报错打扰用户
        if (room->mode() == "local") {
            const std::string itemId = message.value("itemId", "");

            // itemId 必须命中列表里真实存在的条目。Room::files 的键完全来自
            // 客户端，且没有任何清理路径（dropMemberFiles 只删内层 clientId，
            // 外层空 map 留着），不校验就等于让对端随意往服务端内存里塞键：
            // 实测 3000 条带 4KB 伪造 itemId 的消息可让常驻内存 +13 MB，
            // 且永不释放。条目可能刚被删掉，所以静默忽略而不报错。
            const auto &entries = room->playlist();
            if (itemId.empty()
                || std::none_of(entries.begin(), entries.end(),
                                [&itemId](const PlaylistEntry &e) {
                                    return e.itemId == itemId;
                                })) {
                return;
            }

            room->setMemberFile(room->clientIdOf(session), itemId,
                                message.value("hasFile", false),
                                message.value("hash", ""));
            broadcastPlaylistStatus(room);
        }
        return;
    }

    if (type == "video_status") {
        auto roomIt = m_sessionRooms.find(session);
        if (roomIt == m_sessionRooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
            return;
        }
        auto it = m_rooms.find(roomIt->second);
        if (it == m_rooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
            return;
        }
        const auto room = it->second;

        const bool loaded = message.value("loaded", false);
        const std::string hash = message.value("hash", "");
        const long long duration = message.value("duration", 0LL);
        const std::string itemId = message.value("itemId", "");
        const long long seq = message.value("seq", 0LL);

        // 多个工作线程从同一队列取消息，同一客户端的 video_status 可能晚于
        // set_room_mode / playlist_switch 才被处理。非当前条目的状态不再写入
        // 成员表，否则会把新条目的 loaded/hash 覆盖回旧值。
        if (itemId != room->currentItemId()) {
            LOG_DEBUG("Logic") << "忽略旧条目的视频状态: clientId="
                               << room->clientIdOf(session)
                               << " itemId=" << (itemId.empty() ? "-" : itemId.substr(0, 8))
                               << " current="
                               << (room->currentItemId().empty()
                                       ? "-"
                                       : room->currentItemId().substr(0, 8));
            return;
        }

        // 同一连接内的乱序（例如「卸载旧条目」晚于「新条目已加载」）按序号丢弃；
        // 不中继也不重算，接收端看到的始终是最新状态。
        if (!room->setMemberVideo(session, loaded, hash, duration, itemId, seq))
            return;

        // 中继给其他人（不含哈希，哈希只留在服务端做比对）
        json relay;
        relay["type"] = "video_status";
        relay["clientId"] = room->clientIdOf(session);
        relay["loaded"] = loaded;
        relay["duration"] = duration;
        relay["itemId"] = itemId;
        room->broadcast(relay, session);

        // 不一致状态变化时通知所有人
        broadcastMismatchIfChanged(room);
        return;
    }

    if (type == "webrtc_offer" || type == "webrtc_answer" || type == "webrtc_ice") {
        auto roomIt = m_sessionRooms.find(session);
        if (roomIt == m_sessionRooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
            return;
        }
        auto it = m_rooms.find(roomIt->second);
        if (it == m_rooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
            return;
        }
        const auto room = it->second;

        // 定向转发：目标必须与发送者在同一个房间
        const std::string targetId = message.value("to", "");
        const auto target = room->findSession(targetId);
        if (!target) {
            // 目标正在离线等待重连：静默丢弃。此时对端本来也收不到，
            // 回 member_not_found 只会让 ICE 候选/重试在客户端刷出一堆弹窗。
            if (room->hasMember(targetId))
                return;
            replyError(session, "member_not_found", "目标成员不存在");
            return;
        }

        json relay = message;
        relay["from"] = room->clientIdOf(session);
        target->send(relay.dump());
        return;
    }

    // ── 播放列表 ──────────────────────────────────────────
    //
    // 列表是房间共享的一份。条目的本地文件路径不在其中，各端自行把 itemId
    // 映射到本机文件。服务端只做三件事：存条目、维护当前条目、广播变化。
    if (type == "playlist_add" || type == "playlist_remove" || type == "playlist_switch") {
        auto roomIt = m_sessionRooms.find(session);
        if (roomIt == m_sessionRooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
            return;
        }
        auto it = m_rooms.find(roomIt->second);
        if (it == m_rooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
            return;
        }
        const auto room = it->second;
        const bool isHostSession = room->isHost(session);

        if (type == "playlist_add") {
            // 共享模式下仅房主可添加：他人的本地文件房主没有，添加后无人能播
            if (room->mode() == "share" && !isHostSession) {
                replyError(session, "not_host", "共享模式下只有房主能添加视频");
                return;
            }

            PlaylistEntry entry;
            entry.itemId = message.value("itemId", "");
            entry.title = message.value("title", "");
            entry.url = message.value("url", "");
            entry.duration = message.value("duration", 0LL);
            entry.live = message.value("live", false);
            entry.spec = message.value("spec", "");
            entry.addedBy = room->clientIdOf(session);

            if (entry.itemId.empty() || entry.title.empty()) {
                replyError(session, "bad_request", "播放列表条目缺少 itemId 或标题");
                return;
            }

            // itemId 由客户端生成（UUID），服务端只保证不重复
            for (const PlaylistEntry &existing : room->playlist()) {
                if (existing.itemId == entry.itemId) {
                    replyError(session, "duplicate_item", "该条目已在播放列表中");
                    return;
                }
            }

            // 列表长度必须有上限：每次添加都会全量序列化整个列表并群发，
            // 成本随条目数平方增长，而整个过程持有全局房间锁
            if (room->playlist().size() >= Config::MAX_PLAYLIST_ENTRIES) {
                replyError(session, "playlist_full", "播放列表条目已达上限");
                return;
            }

            const bool becameCurrent = room->addEntry(entry);

            // 广播给所有人（含添加者）：各端以服务端的权威列表为准，不做本地乐观插入
            json changed;
            changed["type"] = "playlist_changed";
            changed["items"] = room->playlistJson();
            changed["currentIndex"] = room->currentIndex();
            sendPlaylistMessage(room, changed);

            // 列表原本为空时该条目成为当前条目，各端据此加载
            if (becameCurrent) {
                room->clearMemberVideoStates();
                broadcastPlaylistSwitched(room, room->currentItemId());
                broadcastMismatchIfChanged(room);
            }

            // title 来自客户端，原样写日志会让对端能用换行符伪造日志行、
            // 或用超长标题把 8 MB 的日志文件冲得轮转。截断并剥掉控制字符。
            std::string logTitle = entry.title.substr(0, 120);
            std::replace_if(logTitle.begin(), logTitle.end(),
                            [](unsigned char c) { return c < 0x20 || c == 0x7F; },
                            ' ');
            LOG_INFO("Logic") << "播放列表添加: " << roomIt->second << " ← " << logTitle;
            return;
        }

        // 删除与切集仅房主可操作，同切换模式，属房间级动作
        if (!isHostSession) {
            replyError(session, "not_host", "只有房主能整理播放列表");
            return;
        }

        const std::string itemId = message.value("itemId", "");
        const std::string beforeId = room->currentItemId();

        if (type == "playlist_remove") {
            if (!room->removeEntry(itemId)) {
                replyError(session, "item_not_found", "播放列表中没有该条目");
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
                replyError(session, "item_not_found", "播放列表中没有该条目");
                return;
            }
            LOG_INFO("Logic") << "播放列表切集: " << roomIt->second << " → " << itemId;
        }

        // 仅当前条目变化时广播切换（删除的恰好是当前条目时也会走到这里）
        if (room->currentItemId() != beforeId) {
            room->clearMemberVideoStates();
            broadcastPlaylistSwitched(room, room->currentItemId());
            // 当前条目变了：旧条目的视频状态不再参与一致性比较
            broadcastMismatchIfChanged(room);
        }
        return;
    }

    if (type == "chat" || type == "playback" || type == "playback_position") {
        auto roomIt = m_sessionRooms.find(session);
        if (roomIt == m_sessionRooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
            return;
        }
        auto it = m_rooms.find(roomIt->second);
        if (it == m_rooms.end()) {
            replyError(session, "not_in_room", "当前不在房间中");
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
            // 房主周期性的位置广播：透传并覆盖发送者，不更新房间状态
            json relay = message;
            relay["from"] = nickname;
            room->broadcast(relay, session);
        } else {
            const std::string action = message.value("action", "");
            // 换挡消息可能不带 position（发送方没有已加载的媒体）
            const bool hasPosition = message.contains("position");
            const long long position = message.value("position", -1LL);
            const double rate = message.value("rate", 1.0);

            // 记录房间最后播放状态，新成员加入时据此对齐
            room->updatePlayback(action, position, rate);

            json relay;
            relay["type"] = "playback";
            relay["from"] = nickname;
            relay["action"] = action;
            if (hasPosition)
                relay["position"] = position;
            if (action == "rate")
                relay["rate"] = rate;
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
    // 服务端侧留痕：错误只回给客户端的话，排查问题时这里什么都看不到
    LOG_WARN("Logic") << "拒绝请求: code=" << code << " clientId=" << session->id()
                      << " message=" << message;

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
    // 共享模式下播放列表只有房主持有：条目是他本机的文件，其他人看不到也用不上。
    // 同步/网链模式则是房间共享的一份，发给所有人。
    if (room->mode() == "share") {
        if (const auto host = room->hostSession())
            host->send(message.dump());
        return;
    }
    room->broadcast(message);
}

void LogicSystem::broadcastPlaylistStatus(const std::shared_ptr<Room> &room) {
    // 匹配仅同步模式有意义：共享模式只有房主有文件，网链模式各端读同一 URL
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

void LogicSystem::broadcastMismatchIfChanged(const std::shared_ptr<Room> &room) {
    if (!room->updateMismatch())
        return;

    json notice;
    notice["type"] = "video_mismatch";
    notice["mismatched"] = room->videoMismatched();
    notice["shortestDuration"] = room->shortestDuration();
    room->broadcast(notice);
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

    const auto room = it->second; // 拷贝 shared_ptr，erase 后对象仍存活
    const std::string clientId = room->clientIdOf(session);
    if (clientId.empty())
        return false;

    return removeMemberByClientIdLocked(room, roomId, clientId);
}

bool LogicSystem::removeMemberByClientIdLocked(const std::shared_ptr<Room> &room,
                                               const std::string &roomId,
                                               const std::string &clientId) {
    std::string nickname;
    bool wasHost = false;
    if (!room->removeMemberByClientId(clientId, &nickname, &wasHost))
        return false;

    // 同时清理离开者对播放列表的上报，否则「全员都已加载」会把已离开的成员
    // 也算进去，状态永远不正确
    room->dropMemberFiles(clientId);

    if (room->isEmpty() || wasHost) {
        // 房主离开：解散房间，通知剩余在线成员
        json notice;
        notice["type"] = "room_left";
        for (const auto &memberSession : room->sessions()) {
            memberSession->send(notice.dump());
            m_sessionRooms.erase(memberSession);
        }
        m_rooms.erase(roomId);
        LOG_INFO("Logic") << "房间解散: " << roomId
                          << (wasHost ? "（房主离开）" : "（无剩余成员）")
                          << " 触发者=" << nickname;
    } else {
        json notice;
        notice["type"] = "member_left";
        notice["clientId"] = clientId;
        notice["nickname"] = nickname;
        room->broadcast(notice); // 离开者已移除，自动只发给剩余成员

        // 成员减少，匹配状态需重算（可能从未匹配变为已匹配）
        broadcastPlaylistStatus(room);

        LOG_INFO("Logic") << "成员离开 " << roomId << ": " << nickname
                          << " 房间人数=" << room->sessions().size();

        // 离开的成员可能带走了不一致的视频，重新计算
        broadcastMismatchIfChanged(room);
    }
    return true;
}

void LogicSystem::expireOfflineMembers() {
    std::lock_guard<std::mutex> lock(m_roomsMutex);
    const auto now = std::chrono::steady_clock::now();
    const auto grace = std::chrono::seconds(Config::MEMBER_OFFLINE_GRACE_SECONDS);

    // 先收集再删除：删除可能解散房间，直接遍历 m_rooms 会让迭代器失效
    std::vector<std::pair<std::string, std::string>> expired;
    for (const auto &[roomId, room] : m_rooms) {
        for (const auto &[clientId, offlineAt] : room->offlineMembers()) {
            if (now - offlineAt >= grace)
                expired.emplace_back(roomId, clientId);
        }
    }

    for (const auto &[roomId, clientId] : expired) {
        auto it = m_rooms.find(roomId);
        if (it == m_rooms.end())
            continue; // 房间已经因为其他成员过期而解散
        if (removeMemberByClientIdLocked(it->second, roomId, clientId)) {
            LOG_INFO("Logic") << "离线成员过期移除: roomId=" << roomId
                              << " clientId=" << clientId;
        }
    }
}
