//
// Created by donggu on 2026/9/8.
//

#include "Room.h"
#include "Session.h"

#include <utility>

Room::Room(std::string roomName, std::string password)
    : m_roomName(std::move(roomName)),
      m_password(std::move(password)) {
}

const std::string &Room::roomName() const {
    return m_roomName;
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
    m_members.push_back({std::move(session), std::move(nickname), isHost});
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
