//
// Created by donggu on 2026/9/8.
//

#include "Room.h"
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

void Room::addMember(std::shared_ptr<Session> session,
                     std::string nickname,
                     bool isHost) {
    m_members.push_back({std::move(session), std::move(nickname), isHost, false, "", 0});
}

bool Room::removeMember(const std::shared_ptr<Session> &session) {
    for (auto it = m_members.begin(); it != m_members.end(); ++it) {
        if (it->session == session) {
            m_members.erase(it);
            return true;
        }
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

std::vector<std::shared_ptr<Session>> Room::sessions() const {
    std::vector<std::shared_ptr<Session>> result;
    result.reserve(m_members.size());
    for (const Member &member : m_members)
        result.push_back(member.session);
    return result;
}

std::shared_ptr<Session> Room::findSession(const std::string &clientId) const {
    for (const Member &member : m_members) {
        if (member.session->id() == clientId)
            return member.session;
    }
    return nullptr;
}

std::shared_ptr<Session> Room::hostSession() const {
    for (const Member &member : m_members) {
        if (member.isHost)
            return member.session;
    }
    return nullptr;
}

void Room::setMemberVideo(const std::shared_ptr<Session> &session, bool loaded,
                          const std::string &hash, long long duration) {
    for (Member &member : m_members) {
        if (member.session == session) {
            member.loaded = loaded;
            member.hash = loaded ? hash : std::string();
            member.duration = loaded ? duration : 0;
            return;
        }
    }
}

bool Room::updateMismatch() {
    bool mismatched = false;
    long long shortest = 0;

    std::string commonHash;
    bool haveHash = false;
    long long minDuration = std::numeric_limits<long long>::max();
    long long maxDuration = 0;
    int loadedCount = 0;

    for (const Member &member : m_members) {
        if (!member.loaded)
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

    // 只有一个人加载视频时不存在"不一致"
    if (loadedCount < 2) {
        mismatched = false;
        shortest = 0;
    } else if (minDuration != std::numeric_limits<long long>::max()) {
        shortest = minDuration;
        // 内容相同但时长差异超过 2 秒,视为不同版本(剪辑/编码不同)
        if (maxDuration - minDuration > 2000)
            mismatched = true;
    }

    if (mismatched == m_videoMismatched && shortest == m_shortestDuration)
        return false;

    m_videoMismatched = mismatched;
    m_shortestDuration = shortest;
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

void Room::updatePlayback(const std::string &action, long long position) {
    m_position = position;
    if (action == "play")
        m_playing = true;
    else if (action == "pause")
        m_playing = false;
    // seek 只改位置,不改变播放状态(暂停中拖动进度条不应让房间变成播放中)
}

json Room::membersArray() const {
    json array = json::array();
    for (const Member &member : m_members) {
        array.push_back({
            {"clientId", member.session->id()},
            {"nickname", member.nickname},
            {"isHost", member.isHost},
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
    };

    // 播放列表随房间状态一起下发:新成员入房就能看到队列和"现在放到哪一条",
    // 不需要另开一条同步路径。共享模式下的观众拿空列表(见头文件说明)。
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
    // 只读路径不建表:这个模式还没被写过的话,返回一个空列表
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
    list.files[entry.itemId]; // 建一个空的上报表,后面成员逐个往里填

    // 这个模式的列表还没有"当前条目"(本来是空的)→ 第一条自动成为当前。
    // 否则添加只是往队列后面排队,不打断正在放的片子。
    if (list.currentIndex < 0) {
        list.currentIndex = static_cast<int>(list.entries.size()) - 1;
        return true;
    }
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
            // 删掉的正好是当前条目:顺延到下一条(已经是最后一条就退回新的最后一条)
            list.currentIndex = static_cast<int>(list.entries.size()) - 1;
        }
        return true;
    }
    return false;
}

bool Room::switchTo(const std::string &itemId) {
    Playlist &list = currentPlaylist();
    for (size_t i = 0; i < list.entries.size(); ++i) {
        if (list.entries[i].itemId != itemId)
            continue;
        list.currentIndex = static_cast<int>(i);
        return true;
    }
    return false;
}

void Room::setMemberFile(const std::string &clientId, const std::string &itemId,
                         bool hasFile, const std::string &hash) {
    currentPlaylist().files[itemId][clientId] = MemberFile{hasFile, hash};
}

void Room::dropMemberFiles(const std::string &clientId) {
    for (auto &[mode, list] : m_playlists) {
        for (auto &[itemId, reporters] : list.files)
            reporters.erase(clientId);
    }
}

std::string Room::itemStatus(const std::string &itemId) const {
    const Playlist &list = currentPlaylist();

    if (m_members.empty())
        return "missing";

    const auto itemIt = list.files.find(itemId);
    bool haveHash = false;
    std::string commonHash;

    // 逐成员看:还差一个没文件就是"未匹配";都有文件再比内容哈希
    for (const Member &member : m_members) {
        const std::string clientId = member.session->id();

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
            // 匹配状态由服务端聚合(它才知道所有成员的情况),客户端直接用
            {"status", itemStatus(entry.itemId)},
        });
    }
    return array;
}

void Room::broadcast(const json &message, const std::shared_ptr<Session> &except) {
    for (const Member &member : m_members) {
        if (member.session == except)
            continue;
        member.session->send(message.dump());
    }
}
