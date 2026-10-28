//
// Created by donggu on 2026/9/8.
//

#include "Room.h"
#include "Log.h"
#include "RandomGen.h"
#include "Session.h"

#include <algorithm>
#include <limits>
#include <utility>

Room::Room(std::string roomName, std::string password, std::string mode)
    : m_roomName(std::move(roomName)),
      m_password(std::move(password)),
      m_mode(std::move(mode)) {
}

const std::string &Room::roomName() const {
    return m_roomName;
}

const std::string &Room::mode() const {
    return m_mode;
}

void Room::setMode(const std::string &mode) {
    if (m_mode == mode)
        return;
    LOG_DEBUG("Room") << "模式切换: " << m_mode << " → " << mode;
    m_mode = mode;
}

bool Room::hasPassword() const {
    return !m_password.empty();
}

bool Room::checkPassword(const std::string &password) const {
    return m_password == password;
}

bool Room::isEmpty() const {
    return m_members.empty();
}

std::size_t Room::onlineCount() const {
    return static_cast<std::size_t>(
        std::count_if(m_members.begin(), m_members.end(),
                      [](const Member &member) { return member.online; }));
}

std::size_t Room::memberCount() const {
    return m_members.size();
}

void Room::addMember(std::shared_ptr<Session> session,
                     std::string nickname,
                     bool isHost) {
    LOG_DEBUG("Room") << "成员登记: " << nickname << " clientId=" << session->id()
                      << (isHost ? " 房主" : " 观众");
    Member member;
    member.clientId = session->id();
    member.session = std::move(session);
    member.nickname = std::move(nickname);
    member.isHost = isHost;
    // 重连凭据：随机 UUID，只下发给本人，不写日志
    member.resumeToken = RandomGen::generateUUID();
    m_members.push_back(std::move(member));
}

bool Room::removeMember(const std::shared_ptr<Session> &session) {
    for (auto it = m_members.begin(); it != m_members.end(); ++it) {
        if (it->session == session) {
            LOG_DEBUG("Room") << "成员移除: " << it->nickname
                              << " clientId=" << it->clientId;
            m_members.erase(it);
            return true;
        }
    }
    LOG_DEBUG("Room") << "成员移除失败，不在成员表中: clientId=" << session->id();
    return false;
}

bool Room::removeMemberByClientId(const std::string &clientId, std::string *nickname,
                                  bool *wasHost) {
    for (auto it = m_members.begin(); it != m_members.end(); ++it) {
        if (it->clientId != clientId)
            continue;
        if (nickname != nullptr)
            *nickname = it->nickname;
        if (wasHost != nullptr)
            *wasHost = it->isHost;
        LOG_DEBUG("Room") << "成员移除(按 clientId): " << it->nickname
                          << " clientId=" << it->clientId;
        m_members.erase(it);
        return true;
    }
    return false;
}

bool Room::isHost(const std::shared_ptr<Session> &session) const {
    for (const Member &member : m_members) {
        if (member.session == session)
            return member.isHost;
    }
    return false;
}

bool Room::hasNickname(const std::string &nickname) const {
    for (const Member &member : m_members) {
        if (member.nickname == nickname)
            return true;
    }
    return false;
}

std::string Room::nicknameOf(const std::shared_ptr<Session> &session) const {
    for (const Member &member : m_members) {
        if (member.session == session)
            return member.nickname;
    }
    return std::string();
}

std::string Room::clientIdOf(const std::shared_ptr<Session> &session) const {
    for (const Member &member : m_members) {
        if (member.session == session)
            return member.clientId;
    }
    return std::string();
}

std::vector<std::shared_ptr<Session>> Room::sessions() const {
    std::vector<std::shared_ptr<Session>> result;
    result.reserve(m_members.size());
    for (const Member &member : m_members) {
        if (member.online)
            result.push_back(member.session);
    }
    return result;
}

std::shared_ptr<Session> Room::findSession(const std::string &clientId) const {
    for (const Member &member : m_members) {
        if (member.online && member.clientId == clientId)
            return member.session;
    }
    return nullptr;
}

bool Room::hasMember(const std::string &clientId) const {
    for (const Member &member : m_members) {
        if (member.clientId == clientId)
            return true;
    }
    return false;
}

std::shared_ptr<Session> Room::hostSession() const {
    for (const Member &member : m_members) {
        if (member.isHost && member.online)
            return member.session;
    }
    return nullptr;
}

bool Room::markMemberOffline(const std::shared_ptr<Session> &session,
                             std::chrono::steady_clock::time_point now) {
    for (Member &member : m_members) {
        if (member.session != session)
            continue;
        if (!member.online)
            return false;
        member.online = false;
        member.offlineAt = now;
        LOG_INFO("Room") << "成员离线(保留会话): " << member.nickname
                         << " clientId=" << member.clientId;
        return true;
    }
    return false;
}

std::shared_ptr<Session> Room::resumeMember(
    const std::string &clientId, const std::string &token,
    const std::shared_ptr<Session> &newSession) {
    for (Member &member : m_members) {
        if (member.clientId != clientId || member.resumeToken != token)
            continue;

        auto oldSession = member.session;
        member.session = newSession;
        member.online = true;
        member.offlineAt = {};
        LOG_INFO("Room") << "成员恢复: " << member.nickname
                         << " clientId=" << member.clientId;
        return oldSession;
    }
    return nullptr;
}

std::string Room::resumeTokenFor(const std::shared_ptr<Session> &session) const {
    for (const Member &member : m_members) {
        if (member.session == session)
            return member.resumeToken;
    }
    return std::string();
}

std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>>
Room::offlineMembers() const {
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> result;
    for (const Member &member : m_members) {
        if (!member.online)
            result.emplace_back(member.clientId, member.offlineAt);
    }
    return result;
}

bool Room::setMemberVideo(const std::shared_ptr<Session> &session, bool loaded,
                          const std::string &hash, long long duration,
                          const std::string &itemId, long long seq) {
    for (Member &member : m_members) {
        if (member.session == session) {
            if (seq > 0 && seq <= member.videoSeq) {
                LOG_DEBUG("Room") << "忽略乱序的视频状态: " << member.nickname
                                  << " seq=" << seq << " last=" << member.videoSeq;
                return false;
            }
            member.videoSeq = seq;
            LOG_DEBUG("Room") << "成员视频状态: " << member.nickname
                              << " loaded=" << (loaded ? 1 : 0)
                              << " duration=" << duration
                              << " itemId=" << (itemId.empty() ? "-" : itemId.substr(0, 8))
                              << " hash=" << (hash.empty() ? "-" : hash.substr(0, 8));
            member.loaded = loaded;
            member.hash = loaded ? hash : std::string();
            member.duration = loaded ? duration : 0;
            member.itemId = loaded ? itemId : std::string();
            return true;
        }
    }
    LOG_DEBUG("Room") << "视频状态上报来自非成员: clientId=" << session->id();
    return false;
}

void Room::clearMemberVideoStates() {
    for (Member &member : m_members) {
        member.loaded = false;
        member.hash.clear();
        member.duration = 0;
        member.itemId.clear();
    }
    LOG_DEBUG("Room") << "清空成员视频状态：当前条目已变化";
}

bool Room::updateMismatch() {
    bool mismatched = false;
    long long shortest = 0;

    // 只比较「当前条目」的状态。切换模式/切条目时，成员可能还持有上一条
    // 的 loaded/hash/duration；不按 itemId 过滤就会把不同视频误判为不一致。
    const std::string currentItem = currentItemId();

    std::string commonHash;
    bool haveHash = false;
    long long minDuration = std::numeric_limits<long long>::max();
    long long maxDuration = 0;
    int loadedCount = 0;

    for (const Member &member : m_members) {
        if (!member.loaded)
            continue;
        if (currentItem.empty() || member.itemId != currentItem)
            continue;

        ++loadedCount;

        if (!member.hash.empty()) {
            if (!haveHash) {
                commonHash = member.hash;
                haveHash = true;
            } else if (member.hash != commonHash) {
                mismatched = true; // 内容哈希不同
            }
        }

        if (member.duration > 0) {
            minDuration = std::min(minDuration, member.duration);
            maxDuration = std::max(maxDuration, member.duration);
        }
    }

    // 只有一人加载视频时不存在不一致
    if (loadedCount < 2) {
        mismatched = false;
        shortest = 0;
    } else if (minDuration != std::numeric_limits<long long>::max()) {
        shortest = minDuration;
        // 内容相同但时长差异超过 2 秒，视为不同版本（剪辑/编码不同）
        if (maxDuration - minDuration > 2000)
            mismatched = true;
    }

    if (mismatched == m_videoMismatched && shortest == m_shortestDuration)
        return false;

    m_videoMismatched = mismatched;
    m_shortestDuration = shortest;

    LOG_INFO("Room") << "视频一致性变化: mismatched=" << (mismatched ? 1 : 0)
                     << " shortestDuration=" << shortest
                     << " loadedCount=" << loadedCount;
    return true;
}

bool Room::videoMismatched() const {
    return m_videoMismatched;
}

long long Room::shortestDuration() const {
    return m_shortestDuration;
}

bool Room::playing() const {
    return m_playing;
}

long long Room::position() const {
    return m_position;
}

void Room::updatePlayback(const std::string &action, long long position, double rate) {
    LOG_DEBUG("Room") << "播放状态更新: action=" << action << " position=" << position
                      << (action == "rate" ? " rate=" + std::to_string(rate) : "");
    // position < 0 表示发送方位置未知（换挡时播放器尚未加载），不改房间位置
    if (position >= 0)
        m_position = position;
    if (action == "play")
        m_playing = true;
    else if (action == "pause")
        m_playing = false;
    else if (action == "rate")
        m_rate = std::clamp(rate, 0.25, 4.0);
    // seek 只改位置，不改变播放状态（暂停中拖动进度条不应使房间变为播放中）
}

json Room::membersArray() const {
    json array = json::array();
    for (const Member &member : m_members) {
        array.push_back({
            {"clientId", member.clientId},
            {"nickname", member.nickname},
            {"isHost", member.isHost},
            {"online", member.online},
            {"loaded", member.loaded},
            {"duration", member.duration},
        });
    }
    return array;
}

json Room::stateJson(bool includePlaylist) const {
    const Playlist &list = currentPlaylist();
    json state = {
        {"playing", m_playing},
        {"position", m_position},
        {"rate", m_rate},
    };

    // 播放列表随房间状态一起下发，新成员入房即可看到队列与当前条目，
    // 无需另开一条同步路径。共享模式下的观众为空列表（见头文件说明）。
    state["playlist"] = includePlaylist ? playlistJson() : json::array();
    state["currentIndex"] = includePlaylist ? list.currentIndex : -1;
    return state;
}

// ============================
// 播放列表
// ============================

Playlist &Room::currentPlaylist() {
    return m_playlists[m_mode];
}

const Playlist &Room::currentPlaylist() const {
    // 只读路径不建表：该模式尚未被写过时返回一个空列表
    static const Playlist kEmpty;
    const auto it = m_playlists.find(m_mode);
    return it != m_playlists.end() ? it->second : kEmpty;
}

const std::vector<PlaylistEntry> &Room::playlist() const {
    return currentPlaylist().entries;
}

int Room::currentIndex() const {
    return currentPlaylist().currentIndex;
}

std::string Room::currentItemId() const {
    const Playlist &list = currentPlaylist();
    if (list.currentIndex < 0 || list.currentIndex >= static_cast<int>(list.entries.size()))
        return {};
    return list.entries[list.currentIndex].itemId;
}

bool Room::addEntry(const PlaylistEntry &entry) {
    Playlist &list = currentPlaylist();
    list.entries.push_back(entry);
    list.files[entry.itemId]; // 建一个空的上报表，成员后续逐个填入

    // 该模式的列表原本为空时，第一条自动成为当前条目；
    // 否则仅追加到队列末尾，不打断正在播放的条目。
    if (list.currentIndex < 0) {
        list.currentIndex = static_cast<int>(list.entries.size()) - 1;
        LOG_DEBUG("Room") << "条目加入并成为当前(" << m_mode << "): " << entry.itemId
                          << " " << entry.title;
        return true;
    }
    LOG_DEBUG("Room") << "条目加入(" << m_mode << "): " << entry.itemId << " " << entry.title;
    return false;
}

bool Room::removeEntry(const std::string &itemId) {
    Playlist &list = currentPlaylist();
    for (auto it = list.entries.begin(); it != list.entries.end(); ++it) {
        if (it->itemId != itemId)
            continue;

        const int removedIndex = static_cast<int>(std::distance(list.entries.begin(), it));
        list.entries.erase(it);
        list.files.erase(itemId);

        if (list.entries.empty()) {
            list.currentIndex = -1;
        } else if (removedIndex < list.currentIndex) {
            --list.currentIndex;
        } else if (removedIndex == list.currentIndex
                   && list.currentIndex >= static_cast<int>(list.entries.size())) {
            // 删除的是当前条目：顺延到下一条，已是最后一条则退回新的最后一条
            list.currentIndex = static_cast<int>(list.entries.size()) - 1;
        }
        LOG_DEBUG("Room") << "条目移除(" << m_mode << "): " << itemId
                          << " 剩余=" << list.entries.size()
                          << " currentIndex=" << list.currentIndex;
        return true;
    }
    LOG_WARN("Room") << "条目移除失败，列表中没有该条目(" << m_mode << "): " << itemId;
    return false;
}

bool Room::switchTo(const std::string &itemId) {
    Playlist &list = currentPlaylist();
    for (size_t i = 0; i < list.entries.size(); ++i) {
        if (list.entries[i].itemId != itemId)
            continue;
        list.currentIndex = static_cast<int>(i);
        LOG_DEBUG("Room") << "条目切换(" << m_mode << "): " << itemId
                          << " index=" << list.currentIndex;
        return true;
    }
    LOG_WARN("Room") << "条目切换失败，列表中没有该条目(" << m_mode << "): " << itemId;
    return false;
}

void Room::setMemberFile(const std::string &clientId, const std::string &itemId,
                         bool hasFile, const std::string &hash) {
    LOG_DEBUG("Room") << "文件上报: itemId=" << itemId << " clientId=" << clientId
                      << " hasFile=" << (hasFile ? 1 : 0)
                      << " hash=" << (hash.empty() ? "-" : hash.substr(0, 8));
    currentPlaylist().files[itemId][clientId] = MemberFile{hasFile, hash};
}

void Room::dropMemberFiles(const std::string &clientId) {
    for (auto &[mode, list] : m_playlists) {
        for (auto &[itemId, reporters] : list.files)
            reporters.erase(clientId);
    }
    LOG_DEBUG("Room") << "清理成员文件上报: clientId=" << clientId;
}

std::string Room::itemStatus(const std::string &itemId) const {
    const Playlist &list = currentPlaylist();

    if (m_members.empty())
        return "missing";

    const auto itemIt = list.files.find(itemId);
    bool haveHash = false;
    std::string commonHash;

    // 逐个成员检查：任一人没有文件即为未匹配；全员都有再比对内容哈希。
    // 离线等待重连的成员沿用其离开前的上报，避免房间门禁在短暂断线时抖动。
    for (const Member &member : m_members) {
        const std::string clientId = member.clientId;

        bool hasFile = false;
        std::string hash;
        if (itemIt != list.files.end()) {
            const auto reportIt = itemIt->second.find(clientId);
            if (reportIt != itemIt->second.end()) {
                hasFile = reportIt->second.hasFile;
                hash = reportIt->second.hash;
            }
        }

        if (!hasFile)
            return "missing";

        if (!hash.empty()) {
            if (!haveHash) {
                commonHash = hash;
                haveHash = true;
            } else if (hash != commonHash) {
                return "mismatch";
            }
        }
    }

    return "matched";
}

json Room::playlistJson() const {
    json array = json::array();
    for (const PlaylistEntry &entry : currentPlaylist().entries) {
        array.push_back({
            {"itemId", entry.itemId},
            {"title", entry.title},
            {"url", entry.url},
            {"duration", entry.duration},
            {"addedBy", entry.addedBy},
            {"live", entry.live},
            {"spec", entry.spec},
            // 匹配状态由服务端聚合（只有它掌握所有成员的情况），客户端直接使用
            {"status", itemStatus(entry.itemId)},
        });
    }
    return array;
}

void Room::broadcast(const json &message, const std::shared_ptr<Session> &except) {
    const std::string payload = message.dump();
    for (const Member &member : m_members) {
        // 离线成员的 Session 已经关闭，不能继续往写队列里塞消息
        if (!member.online || member.session == except)
            continue;
        member.session->send(payload);
    }
}
