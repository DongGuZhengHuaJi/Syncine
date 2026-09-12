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

json Room::stateJson() const {
    return {
        {"playing", m_playing},
        {"position", m_position},
    };
}

void Room::broadcast(const json &message, const std::shared_ptr<Session> &except) {
    for (const Member &member : m_members) {
        if (member.session == except)
            continue;
        member.session->send(message.dump());
    }
}
