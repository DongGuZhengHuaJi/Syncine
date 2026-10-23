//
// 房间会话：只负责「已进入房间」之后的事务。
//
// 明确不属于此处的职责：
//   * 建连、重连、create/join 请求的重试与等待   → SessionController
//   * 本地播放器门禁、回声抑制、video_status 上报 → PlaybackSync
//   * WebRTC 信令的目标解析与转发                → SignalingChannel
//   * JSON 编解码                               → Protocol
//
// 因此它不认识 QMediaPlayer，不认识 QWebSocket（只依赖一个「能发消息」的接口），
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

    // 播放列表（房间共享的队列 + 本机自己的文件映射）,同上
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
    // 三种观影模式：同步 / 房主共享媒体流 / 网链
    //
    // 同步模式指各端各自持有同一部片子（原称「本地模式」），播放状态实时同步。
    // 线路格式仍为 "local"——服务端不认识「同步」这个名字，改名只涉及客户端一侧，
    // 映射集中在 modeToWire / modeFromWire 两个函数中。
    enum class RoomMode {
        Sync,
        Share,
        Url
    };
    Q_ENUM(RoomMode)

    enum class PlaybackAction {
        Play,
        Pause,
        Seek,
        Rate    // 换挡：rate 字段有效，position 用于顺带对齐
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
    // 某成员当前是否在线（断线等待重连期间为 false）
    bool isMemberOnline(const QString &clientId) const;
    MemberModel *members() const;
    PlaylistModel *playlist() const;
    bool videoMismatched() const;
    qint64 shortestDuration() const;

    // 某条目在本机对应的文件路径（空 = 没有）。PlaybackSync 解析播放源时使用
    QString localFileFor(const QString &itemId) const;
    // 某条目的网链地址（空 = 该条目没有 URL）
    QString urlFor(const QString &itemId) const;

    // 除自身外的成员是否都已加载视频（同步模式门禁使用）
    bool allOthersLoaded() const;

    // 除自身外的全部成员 ID。网状网需为每个对端各建一条 WebRTC 连接，
    // 故此处返回列表而非单个 ID。
    QList<QString> otherMemberIds() const;

    // 入房时服务端下发的 ICE 配置（STUN + 带临时凭据的 TURN）。空 = 服务端未提供
    // （旧版服务端，或它自身也拿不到凭据）。
    //
    // 本类只保管与转交，不认识 WebRTC——将其转换为 PeerConnection 配置是
    // SignalingChannel + WebrtcManager 的职责（与「播放列表条目不认识播放器」
    // 同理）。
    QList<IceServerConfig> iceServers() const;

    // ---- 生命周期：由 SessionController 在入场/离场时调用 ----
    void enterRoom(const RoomSnapshot &snapshot);
    void leaveRoom();

    // ---- QML 调用的房间内操作 ----
    Q_INVOKABLE void setRoomMode(RoomMode mode);
    Q_INVOKABLE void sendChat(const QString &text);

    // ---- 播放列表（QML 调用） ----
    //
    // 本地文件路径不进协议：addLocalFile / assignLocalFile 只把路径记在本地
    // （itemId → 本机文件），服务端只收到条目标题。
    // 同步模式下每人都要为自己的条目选一次文件；共享模式下只有房主的映射有意义。
    // 参数为 QUrl 而非 QString：QML 的 FileDialog.selectedFile 是 url 类型，
    // 传 QString 会被转成 "file:///…" 字符串，后续作为路径使用（读文件、算哈希）
    // 将全部失效——媒体加载失败、匹配状态永远为未匹配即由此而来。
    // 让 QML 原样传 QUrl，C++ 侧用 toLocalFile() 取得干净路径。
    Q_INVOKABLE void addLocalFile(const QUrl &fileUrl);
    // live / durationMs / spec 均由探测（UrlInspector/UrlDownloader）得到：
    //   直播无时长，也不能下载合并；
    //   一并带上 durationMs，列表便无需先显示 "--:--" 等待有人加载完成；
    //   spec 是画质选择（条目级，语义见 FormatModel.h）——各端按同一条 spec
    //   各自下载，选择不一致会导致内容对不上、被判为不同步。
    //
    // itemId 由调用方提供：同一内容在不同客户端必须得到同一个 id，否则服务端
    // 查重（按 itemId 比对）无法识别，房间列表中会出现两条相同条目。
    // 自定义链接由 URL 计算（见下），剧集用集号（见 FormatModel 的 Episodes::itemId）。
    //
    // 注意：QML 调用此方法必须传满全部参数——Q_INVOKABLE 的默认参数不存在于
    // meta-object 中，QML 少传一个会直接报 "Insufficient arguments"。
    Q_INVOKABLE void addUrlWithId(const QString &itemId, const QString &url,
                                  const QString &title, bool live,
                                  qint64 durationMs, const QString &spec);
    // 自定义链接的便捷入口：itemId 由 URL 计算得出（同一链接即同一条，各端一致）
    Q_INVOKABLE void addUrl(const QString &url, const QString &title, bool live,
                            qint64 durationMs, const QString &spec);
    Q_INVOKABLE void assignLocalFile(const QString &itemId, const QUrl &fileUrl);
    Q_INVOKABLE void removeFromPlaylist(const QString &itemId);
    Q_INVOKABLE void switchTo(const QString &itemId);

    // ---- 供 PlaybackSync 调用 ----
    void sendPlayback(PlaybackAction action, qint64 position, qreal rate = 1.0);
    void reportVideoStatus(bool loaded, const QString &hash, qint64 duration);

    // 房主周期性广播自身播放位置，观众端据此绘制进度条。
    // 仅房主/推流方调用，且仅在共享/网链模式下有意义。
    void sendPlaybackPosition(qint64 position, bool playing);

signals:
    void roomIdChanged();
    void roomNameChanged();
    void inRoomChanged();
    void clientIdChanged();
    void roomModeChanged();
    void isHostChanged();
    void videoMismatchChanged();

    // 成员表内容变化（增删改），供 C++ 侧重算派生量
    void membersChanged();

    void entered();
    void left();

    void memberJoined(const QString &nickname);
    void memberLeft(const QString &nickname);
    // 断线重连的成员恢复在线（clientId 不变）
    void memberOnline(const QString &clientId);

    void chatReceived(const QString &from, const QString &text);
    void playbackCommandReceived(RoomSession::PlaybackAction action, qint64 position,
                                 qreal rate);

    // 当前条目变化（或原条目刚被指定本机文件），各端据此重新解析该条目的来源：
    // 同步模式与共享模式下的房主读取本地文件，网链模式读取 url，观众端不处理。
    // itemId 为空表示当前无条目（列表已空），此时应卸载播放器。
    void playlistSwitched(const QString &itemId);

    // 收到房主的位置广播（非命令——仅用于更新界面，不驱动本地播放器）
    void playbackPositionReceived(qint64 position, bool playing);

    void errorOccurred(const QString &message);

private slots:
    void onMessage(const QString &text);

private:
    void handleMemberJoined(const Protocol::Message &message);
    void handleMemberLeft(const Protocol::Message &message);
    void handleMemberOnline(const Protocol::Message &message);
    void handlePlayback(const Protocol::Message &message);
    void handleVideoStatus(const Protocol::Message &message);
    void handleVideoMismatch(const Protocol::Message &message);
    void handlePlaylistChanged(const Protocol::Message &message);
    void handlePlaylistSwitched(const Protocol::Message &message);
    void handlePlaylistStatus(const Protocol::Message &message);
    // 上报「该条目本机是否有文件」（仅同步模式发送，服务端据此计算匹配状态）
    void reportPlaylistFile(const QString &itemId, const QString &path);

    // 成员表变化后统一重算派生量，避免各分支重复 emit
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
    QList<IceServerConfig> m_iceServers;
};

#endif //SYNCINE_ROOMSESSION_H
