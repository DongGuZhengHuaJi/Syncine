//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_ROOM_H
#define SERVER_ROOM_H

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class Session;

using json = nlohmann::json;

// 播放列表里的一条。不带本地文件路径——那是各端自己的映射：
// 本地模式映射到本机文件，共享模式只有房主的映射有意义，网链模式直接用 url。
// 服务端只存各端共同认可的条目定义。
struct PlaylistEntry {
    std::string itemId;   // 客户端生成的 UUID，服务端只保证不重复
    std::string title;    // 显示名：本地/共享条目是文件名，网链条目是标题
    std::string url;      // 可选，网链模式的源
    long long duration = 0; // 由 video_status 回报，0 表示未知
    std::string addedBy;  // 添加者的 clientId
    // 直播：无时长、不能下载合并，各端用 QMediaPlayer 直接拉流。
    // 服务端只存该标志并转发，不解释。
    bool live = false;
    // 画质选择（客户端定义的 spec 字符串，语义见客户端 FormatModel.h）。
    // 条目级而非个人偏好：各端选择不一致会导致内容哈希不同、被判为不同步。
    // 服务端只存转发，不解释内容——客户端改选择方式无需服务端跟着改。
    std::string spec;
};

// 某成员对某条目「本机是否有对应文件」的上报，仅同步模式使用。
// 共享模式只有房主的文件有意义；网链模式各端读同一 URL，都不存在「匹配」。
struct MemberFile {
    bool hasFile = false;
    std::string hash;   // 内容哈希（与 video_status 同一套算法），用于发现不同步
};

// 一个模式下的播放列表。三种模式各存一份：换模式时列表跟着换，切回时仍保留。
// 同步模式是「各端都有这几部片子」，共享模式是「房主有这几部片子」。
struct Playlist {
    std::vector<PlaylistEntry> entries;
    int currentIndex = -1;
    // itemId → clientId → 该成员这一条的本机文件情况
    std::map<std::string, std::map<std::string, MemberFile>> files;
};

// 一个房间：成员列表、密码、模式、播放状态、成员视频信息、播放列表
class Room {
public:
    Room(std::string roomName, std::string password, std::string mode = "local");

    const std::string &roomName() const;
    const std::string &mode() const;
    void setMode(const std::string &mode);

    bool hasPassword() const;
    bool checkPassword(const std::string &password) const;
    bool isEmpty() const;

    void addMember(std::shared_ptr<Session> session, std::string nickname, bool isHost);
    // 返回是否真的移除了成员
    bool removeMember(const std::shared_ptr<Session> &session);
    // 按稳定 clientId 移除（离线成员过期时用）
    bool removeMemberByClientId(const std::string &clientId, std::string *nickname,
                                bool *wasHost);
    bool isHost(const std::shared_ptr<Session> &session) const;
    bool hasNickname(const std::string &nickname) const;
    std::string nicknameOf(const std::shared_ptr<Session> &session) const;
    // 会话对应的稳定成员 id（重连后不变）。不在成员表中返回空串。
    std::string clientIdOf(const std::shared_ptr<Session> &session) const;
    // 在线成员数 / 全部成员数（含离线等待重连的）
    std::size_t onlineCount() const;
    std::size_t memberCount() const;
    std::vector<std::shared_ptr<Session>> sessions() const;
    // 按 clientId 查找**在线**成员（用于 WebRTC 信令等定向转发）
    std::shared_ptr<Session> findSession(const std::string &clientId) const;
    // 房主的会话（房主离线时返回 nullptr）
    std::shared_ptr<Session> hostSession() const;

    // ---- 断线重连（2A）----
    // 标记成员离线，保留其成员记录/MemberFile/播放列表上报；返回是否找到
    bool markMemberOffline(const std::shared_ptr<Session> &session,
                           std::chrono::steady_clock::time_point now);
    // 用 token 恢复成员：换上新连接、保留原 clientId/昵称/房主身份/上报状态。
    // 成功返回旧会话（调用方负责关闭并清理映射），失败返回 nullptr。
    std::shared_ptr<Session> resumeMember(const std::string &clientId,
                                          const std::string &token,
                                          const std::shared_ptr<Session> &newSession);
    std::string resumeTokenFor(const std::shared_ptr<Session> &session) const;
    // 离线成员列表：clientId + 离线时刻
    std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>>
    offlineMembers() const;

    // 成员视频信息（加载状态/内容哈希/时长）
    void setMemberVideo(const std::shared_ptr<Session> &session, bool loaded,
                        const std::string &hash, long long duration);

    // 重新计算成员视频是否一致；返回状态是否发生变化
    bool updateMismatch();
    bool videoMismatched() const;
    long long shortestDuration() const;

    // 播放状态（用于新成员加入时对齐）
    bool playing() const;
    long long position() const;
    // rate 只有 action == "rate" 时才会写进房间状态
    void updatePlayback(const std::string &action, long long position, double rate);

    // ---- 播放列表（都作用于当前模式那一份）----
    const std::vector<PlaylistEntry> &playlist() const;
    int currentIndex() const;
    // 当前条目的 itemId；列表为空时返回空串
    std::string currentItemId() const;
    // 追加到末尾。列表原本没有当前条目时新条目自动成为当前并返回 true，
    // 调用方据此补发一条 playlist_switched
    bool addEntry(const PlaylistEntry &entry);
    // 条目不存在返回 false
    bool removeEntry(const std::string &itemId);
    // 条目存在就切过去（已是当前条目也算成功）；不存在返回 false
    bool switchTo(const std::string &itemId);

    // 成员上报「这一条本机是否有文件」（仅同步模式）
    void setMemberFile(const std::string &clientId, const std::string &itemId,
                       bool hasFile, const std::string &hash);
    // 成员离开时清掉其全部上报，否则「全员都已加载」会把已离开的成员算进去
    void dropMemberFiles(const std::string &clientId);
    // 条目在当前成员下的匹配状态：missing 尚有人无文件 / matched 全员一致 /
    // mismatch 全员都有但内容不一致
    std::string itemStatus(const std::string &itemId) const;

    // 协议序列化
    json membersArray() const;
    json playlistJson() const;
    // includePlaylist=false 用于共享模式下的观众：条目的本地文件是房主的，
    // 对他们没有意义，返回空列表即可
    json stateJson(bool includePlaylist = true) const;

    // 广播给所有成员（except 不回发，通常传发送者）
    void broadcast(const json &message, const std::shared_ptr<Session> &except = nullptr);

private:
    struct Member {
        // 稳定成员 id：首次进房时由 session->id() 生成，重连后不变。
        // 对端、播放列表、文件上报全部用它，而不是连接本身的 id。
        std::string clientId;
        std::shared_ptr<Session> session;
        std::string nickname;
        bool isHost;
        // 断线等待重连期间 member 仍在表里，但不参与广播、不能收信令。
        bool online = true;
        std::chrono::steady_clock::time_point offlineAt;
        // 重连凭据。不下发给其他成员，也不写日志。
        std::string resumeToken;
        bool loaded = false;
        std::string hash;
        long long duration = 0;
    };

    std::string m_roomName;
    std::string m_password;
    std::string m_mode;
    std::vector<Member> m_members;

    bool m_playing = false;
    long long m_position = 0;
    double m_rate = 1.0;

    bool m_videoMismatched = false;
    long long m_shortestDuration = 0;

    // mode → 该模式下的播放列表（三种模式各存一份，互不干扰）
    std::map<std::string, Playlist> m_playlists;

    Playlist &currentPlaylist();
    const Playlist &currentPlaylist() const;
};

#endif //SERVER_ROOM_H
