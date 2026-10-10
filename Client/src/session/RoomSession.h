//
// 房间会话 —— 只管「已经在房间里」之后的事。
//
// 明确不属于这里的职责:
//   * 建连、重连、create/join 请求的重试与等待   -> SessionController
//   * 本地播放器的门禁、回声抑制、video_status 上报 -> PlaybackSync
//   * WebRTC 信令的目标解析与转发                 -> SignalingChannel
//   * JSON 编解码                                -> Protocol
//
// 所以它不认识 QMediaPlayer,不认识 QWebSocket(只认识一个"能发消息"的接口),
// 也不知道 WebRTC 的存在。
//

#ifndef SYNCINE_ROOMSESSION_H
#define SYNCINE_ROOMSESSION_H

#include <QObject>
#include <QString>
#include <QUrl>

#include <qqmlintegration.h>

#include "core/Protocol.h"
#include "core/RoomTypes.h"
#include "session/MemberModel.h"
#include "session/PlaylistModel.h"

class NetworkManager;

class RoomSession : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("由 main 创建并注入")

    Q_PROPERTY(QString roomId
               READ roomId
               NOTIFY roomIdChanged)

    Q_PROPERTY(QString roomName
               READ roomName
               NOTIFY roomNameChanged)

    Q_PROPERTY(bool inRoom
               READ inRoom
               NOTIFY inRoomChanged)

    Q_PROPERTY(QString clientId
               READ clientId
               NOTIFY clientIdChanged)

    Q_PROPERTY(RoomMode roomMode
               READ roomMode
               NOTIFY roomModeChanged)

    Q_PROPERTY(bool isHost
               READ isHost
               NOTIFY isHostChanged)

    // 成员表本身是 model,内容变化由 model 自己的信号通知 QML
    Q_PROPERTY(MemberModel *members
               READ members
               CONSTANT)

    // 播放列表(房间共享的队列 + 本机自己的文件映射),同上
    Q_PROPERTY(PlaylistModel *playlist
               READ playlist
               CONSTANT)

    Q_PROPERTY(bool videoMismatched
               READ videoMismatched
               NOTIFY videoMismatchChanged)

    Q_PROPERTY(qint64 shortestDuration
               READ shortestDuration
               NOTIFY videoMismatchChanged)

public:
    // 三种观影模式:同步 / 房主共享媒体流 / 网链
    //
    // 同步模式 = 大家各自都有同一部片子(以前叫"本地模式"),播放状态实时同步。
    // 线路格式仍是 "local" —— 服务端不认识"同步"这个词,改名只改客户端这一侧,
    // 映射集中在 modeToWire / modeFromWire 两个函数里。
    enum class RoomMode {
        Sync,
        Share,
        Url
    };
    Q_ENUM(RoomMode)

    enum class PlaybackAction {
        Play,
        Pause,
        Seek
    };
    Q_ENUM(PlaybackAction)

    explicit RoomSession(NetworkManager *networkManager, QObject *parent = nullptr);
    ~RoomSession() override = default;

    // ---- 属性 ----
    QString roomId() const;
    QString roomName() const;
    bool inRoom() const;
    QString clientId() const;
    RoomMode roomMode() const;
    bool isHost() const;
    MemberModel *members() const;
    PlaylistModel *playlist() const;
    bool videoMismatched() const;
    qint64 shortestDuration() const;

    // 某条目在本机对应的文件路径(空 = 没有)。PlaybackSync 解析播放源时用
    QString localFileFor(const QString &itemId) const;
    // 某条目的网链地址(空 = 这一条没有 URL)
    QString urlFor(const QString &itemId) const;

    // 除自己之外的成员是否都已加载视频(本地模式门禁用)
    bool allOthersLoaded() const;

    // 除自己之外的全部成员 ID。网状网要为每个对端各建一条 WebRTC 连接,
    // 所以这里返回的是列表而不是单个 ID。
    QList<QString> otherMemberIds() const;

    // ---- 生命周期:由 SessionController 在入场/离场时调用 ----
    void enterRoom(const RoomSnapshot &snapshot);
    void leaveRoom();

    // ---- QML 调用的房间内操作 ----
    Q_INVOKABLE void setRoomMode(RoomMode mode);
    Q_INVOKABLE void sendChat(const QString &text);

    // ---- 播放列表(QML 调用) ----
    //
    // 本地文件路径不进协议:addLocalFile / assignLocalFile 只把路径记在本地
    // (itemId → 本机文件),服务端只收到条目的标题。
    // 本地模式下每个人都要为自己那条选一次文件,共享模式下只有房主的映射有意义。
    // 注意参数是 QUrl 而不是 QString:QML 的 FileDialog.selectedFile 是 url 类型,
    // 传 QString 会被转成 "file:///..." 字符串,后续当路径用(读文件、算哈希)
    // 全部失效 —— 媒体加载失败、匹配状态永远"未匹配"就是这么来的。
    // 让 QML 原样传 QUrl,C++ 里 toLocalFile() 拿干净路径。
    Q_INVOKABLE void addLocalFile(const QUrl &fileUrl);
    Q_INVOKABLE void addUrl(const QString &url, const QString &title);
    Q_INVOKABLE void assignLocalFile(const QString &itemId, const QUrl &fileUrl);
    Q_INVOKABLE void removeFromPlaylist(const QString &itemId);
    Q_INVOKABLE void switchTo(const QString &itemId);

    // ---- 供 PlaybackSync 调用 ----
    void sendPlayback(PlaybackAction action, qint64 position);
    void reportVideoStatus(bool loaded, const QString &hash, qint64 duration);

    // 房主周期性广播自己的播放位置 —— 观众端据此画进度条。
    // 只有房主/推流方调用,且只在共享/网链模式下有意义。
    void sendPlaybackPosition(qint64 position, bool playing);

signals:
    void roomIdChanged();
    void roomNameChanged();
    void inRoomChanged();
    void clientIdChanged();
    void roomModeChanged();
    void isHostChanged();
    void videoMismatchChanged();

    // 成员表内容发生变化(增删改),供 C++ 侧重算派生量
    void membersChanged();

    void entered();
    void left();

    void memberJoined(const QString &nickname);
    void memberLeft(const QString &nickname);

    void chatReceived(const QString &from, const QString &text);
    void playbackCommandReceived(RoomSession::PlaybackAction action, qint64 position);

    // 当前条目变了(或者原本的条目刚被指定了本机文件)—— 各端据此重新解析
    // "这一条从哪儿来":本地/共享房主去读本地文件,网链读 url,观众什么都不做。
    // itemId 为空表示当前没有条目(列表空了),此时应该把播放器卸载掉。
    void playlistSwitched(const QString &itemId);

    // 收到房主的位置广播(不是命令 —— 只用于更新界面,不驱动本地播放器)
    void playbackPositionReceived(qint64 position, bool playing);

    void errorOccurred(const QString &message);

private slots:
    void onMessage(const QString &text);

private:
    void handleMemberJoined(const Protocol::Message &message);
    void handleMemberLeft(const Protocol::Message &message);
    void handlePlayback(const Protocol::Message &message);
    void handleVideoStatus(const Protocol::Message &message);
    void handleVideoMismatch(const Protocol::Message &message);
    void handlePlaylistChanged(const Protocol::Message &message);
    void handlePlaylistSwitched(const Protocol::Message &message);
    void handlePlaylistStatus(const Protocol::Message &message);
    // 上报"这一条我这台机器上有没有文件"(只有同步模式发,服务端靠它算匹配状态)
    void reportPlaylistFile(const QString &itemId, const QString &path);

    // 成员表变化后统一重算派生量,避免每个分支各写一遍 emit
    void refreshDerivedState();
    void clearRoomState();
    bool send(const QString &text);

    static QString modeToWire(RoomMode mode);
    static RoomMode modeFromWire(const QString &mode);
    static QString actionToWire(PlaybackAction action);
    static PlaybackAction actionFromWire(const QString &action);

    NetworkManager *m_networkManager = nullptr;
    MemberModel *m_members = nullptr;
    PlaylistModel *m_playlist = nullptr;

    QString m_roomId;
    QString m_roomName;
    QString m_clientId;
    bool m_inRoom = false;
    bool m_isHost = false;
    RoomMode m_roomMode = RoomMode::Sync;
    bool m_videoMismatched = false;
    qint64 m_shortestDuration = 0;
};

#endif //SYNCINE_ROOMSESSION_H
