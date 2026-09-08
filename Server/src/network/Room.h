//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_ROOM_H
#define SERVER_ROOM_H

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class Session;

using json = nlohmann::json;

// 一个房间:成员列表、密码、播放状态
class Room {
public:
    Room(std::string roomName, std::string password);

    const std::string &roomName() const;

    bool hasPassword() const;
    bool checkPassword(const std::string &password) const;
    bool isEmpty() const;

    void addMember(std::shared_ptr<Session> session, std::string nickname, bool isHost);
    // 返回是否真的移除了成员
    bool removeMember(const std::shared_ptr<Session> &session);
    bool isHost(const std::shared_ptr<Session> &session) const;
    bool hasNickname(const std::string &nickname) const;
    std::string nicknameOf(const std::shared_ptr<Session> &session) const;
    std::vector<std::shared_ptr<Session>> sessions() const;

    // 播放状态(用于新成员加入时对齐)
    bool playing() const;
    long long position() const;
    void updatePlayback(const std::string &action, long long position);

    // 协议序列化
    json membersArray() const;
    json stateJson() const;

    // 广播给所有成员(except 不回发,通常传发送者)
    void broadcast(const json &message, const std::shared_ptr<Session> &except = nullptr);

private:
    struct Member {
        std::shared_ptr<Session> session;
        std::string nickname;
        bool isHost;
    };

    std::string m_roomName;
    std::string m_password;
    std::vector<Member> m_members;

    bool m_playing = false;
    long long m_position = 0;
};

#endif //SERVER_ROOM_H
