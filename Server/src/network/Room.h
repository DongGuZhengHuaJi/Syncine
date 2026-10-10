//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_ROOM_H
#define SERVER_ROOM_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

class Session;

using json = nlohmann::json;

// 播放列表里的一条。
//
// **不带本地文件路径** —— 那是每个人自己的事:
//   本地模式  各人把 itemId 映射到自己机器上的某个文件
//   共享模式  只有房主的映射有意义(它就是推流源)
//   网链模式  直接用 url,不需要映射
// 所以服务端只存"大家共同认可的那一份条目定义"。
struct PlaylistEntry {
    std::string itemId;   // 客户端生成的 UUID,服务端只保证不重复
    std::string title;    // 显示名:本地/共享条目是文件名,网链条目是标题
    std::string url;      // 可选,网链模式的源
    long long duration = 0; // 加载过一次之后才知道(video_status 回报),0 表示未知
    std::string addedBy;  // 添加者的 clientId
};

// 某成员对某条目"我这台机器上有没有对应文件"的上报(只有同步模式用得上):
//   同步模式  每个人都要有一条,谁没有这一条就播不了
//   共享模式  只有房主的文件有意义,不存在"匹配"这回事
//   网链模式  大家读的是同一个 URL,也不需要
struct MemberFile {
    bool hasFile = false;
    std::string hash;   // 内容哈希(和 video_status 用的是同一套算法),用于发现"不同步"
};

// 一个模式下的播放列表。三种模式**各存一份**:换模式时列表跟着换,但各自保留
// (切回来东西还在)。这正是三种模式语义不同的地方 ——
// 同步模式是"我们各自都有这几部片子",共享模式是"房主有这几部片子"。
struct Playlist {
    std::vector<PlaylistEntry> entries;
    int currentIndex = -1;
    // itemId → clientId → 该成员这一条的本机文件情况
    std::map<std::string, std::map<std::string, MemberFile>> files;
};

// 一个房间:成员列表、密码、模式、播放状态、成员视频信息、播放列表
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
    bool isHost(const std::shared_ptr<Session> &session) const;
    bool hasNickname(const std::string &nickname) const;
    std::string nicknameOf(const std::shared_ptr<Session> &session) const;
    std::vector<std::shared_ptr<Session>> sessions() const;
    // 按 clientId 查找成员(用于 WebRTC 信令等定向转发)
    std::shared_ptr<Session> findSession(const std::string &clientId) const;
    // 房主的会话(房间里没有房主时返回 nullptr)
    std::shared_ptr<Session> hostSession() const;

    // 成员视频信息(加载状态/内容哈希/时长)
    void setMemberVideo(const std::shared_ptr<Session> &session, bool loaded,
                        const std::string &hash, long long duration);

    // 重新计算成员视频是否一致;返回状态是否发生变化
    bool updateMismatch();
    bool videoMismatched() const;
    long long shortestDuration() const;

    // 播放状态(用于新成员加入时对齐)
    bool playing() const;
    long long position() const;
    void updatePlayback(const std::string &action, long long position);

    // ---- 播放列表(都作用于"当前模式"那一份)----
    const std::vector<PlaylistEntry> &playlist() const;
    int currentIndex() const;
    // 当前条目的 itemId;没有当前条目(列表空)时返回空串
    std::string currentItemId() const;
    // 追加到末尾。列表原本没有当前条目(currentIndex < 0)时,新条目自动成为当前,
    // 返回 true —— 调用方据此补发一条 playlist_switched
    bool addEntry(const PlaylistEntry &entry);
    bool removeEntry(const std::string &itemId);
    // 条目存在就切过去(已经是当前条目也算成功);不存在返回 false
    bool switchTo(const std::string &itemId);

    // 成员上报"这一条我这台机器上有没有文件"(仅同步模式)
    void setMemberFile(const std::string &clientId, const std::string &itemId,
                       bool hasFile, const std::string &hash);
    // 成员离开/断开时清掉他的全部上报 —— 否则"全员都加载了"会把已经走掉的人也算进去
    void dropMemberFiles(const std::string &clientId);
    // 条目在当前成员下的匹配状态:"missing" 还有人没有 / "matched" 全员都有且内容一致 /
    // "mismatch" 全员都有但内容不一致
    std::string itemStatus(const std::string &itemId) const;

    // 协议序列化
    json membersArray() const;
    json playlistJson() const;
    // includePlaylist=false 用于共享模式下的观众:他们不参与播放列表,
    // 拿到空列表就行(entries 是房主本机的文件,对他们没有意义)
    json stateJson(bool includePlaylist = true) const;

    // 广播给所有成员(except 不回发,通常传发送者)
    void broadcast(const json &message, const std::shared_ptr<Session> &except = nullptr);

private:
    struct Member {
        std::shared_ptr<Session> session;
        std::string nickname;
        bool isHost;
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

    bool m_videoMismatched = false;
    long long m_shortestDuration = 0;

    // mode → 该模式下的播放列表(三种模式各存一份,互不干扰)
    std::map<std::string, Playlist> m_playlists;

    Playlist &currentPlaylist();
    const Playlist &currentPlaylist() const;
};

#endif //SERVER_ROOM_H
