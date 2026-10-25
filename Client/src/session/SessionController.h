//
// 会话控制器：只负责「进入房间的过程」，不负责「房间内的状态」。
//
// 与 RoomSession 分开的原因：建连、等待应答、失败重试属于瞬态过程，
// 而房间成员表属于稳态状态。两者混在一类中会导致同一个 isConnecting
// 同时表示「正在连服务器」与「正在等房间应答」两种含义。
//
// 职责范围：
//   * WebSocket 的连接与断开时机（必要时自动发起连接）
//   * create_room / join_room / leave_room 的请求状态机
//   * welcome / room_created / room_joined / room_left / error 这几类消息
//
// 由于管理的是过程，入场成功后即将结果（RoomSnapshot）交给 RoomSession，
// 自身的待发状态立即清空，此后不再关心房间的变化。
//

#ifndef SYNCINE_SESSIONCONTROLLER_H
#define SYNCINE_SESSIONCONTROLLER_H

#include <QObject>
#include <QString>
#include <QTimer>

#include <qqmlintegration.h>

#include "core/Protocol.h"
#include "core/RoomTypes.h"

class NetworkManager;
class RoomSession;

class SessionController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("由 main 创建并注入")

    // 有请求正在传输，界面据此禁用按钮
    Q_PROPERTY(bool busy
               READ busy
               NOTIFY busyChanged)

    Q_PROPERTY(QString nickname
               READ nickname
               NOTIFY nicknameChanged)

    // 信令断开后保留房间、等待自动回房。QML 用它显示重连提示，
    // 不要在这期间把房间页弹回首页。
    Q_PROPERTY(bool reconnecting
               READ reconnecting
               NOTIFY reconnectingChanged)

    // 自动重连宽限期结束后，是否还能用 resume 恢复上次会话。
    // 条件：不在房间、不在重连中，且手里还有 roomId + 稳定 clientId + token。
    Q_PROPERTY(bool canResumeLastSession
               READ canResumeLastSession
               NOTIFY canResumeLastSessionChanged)
    Q_PROPERTY(QString resumeRoomId
               READ resumeRoomId
               NOTIFY canResumeLastSessionChanged)

public:
    SessionController(NetworkManager *networkManager,
                      RoomSession *roomSession,
                      QObject *parent = nullptr);
    ~SessionController() override = default;

    bool busy() const;
    QString nickname() const;
    bool reconnecting() const { return m_reconnecting; }
    bool canResumeLastSession() const;
    QString resumeRoomId() const;

    Q_INVOKABLE void createRoom(const QString &roomName,
                                const QString &nickname,
                                const QString &password);
    Q_INVOKABLE void joinRoom(const QString &roomId,
                              const QString &nickname,
                              const QString &password);
    Q_INVOKABLE void leaveRoom();
    // 用保存的 clientId + token 恢复上次会话（房主身份也会恢复）
    Q_INVOKABLE void resumeLastSession();

signals:
    void busyChanged();
    void nicknameChanged();
    void reconnectingChanged();
    void canResumeLastSessionChanged();

    void roomCreated();
    void roomJoined();
    void roomResumed();

    void errorOccurred(const QString &errorString);

private slots:
    void onMessage(const QString &text);
    void onConnected();
    void onDisconnected();

private:
    enum class PendingRequest {
        None,
        CreateRoom,
        JoinRoom,
        ResumeLastRoom
    };

    void handleRoomEntered(const Protocol::Message &message);
    void sendPendingRequest();
    void failPending(const QString &errorString);
    void setAwaitingReply(bool awaiting);
    RoomSnapshot snapshotFrom(const Protocol::Message &message) const;

    // ---- 断线重连 ----
    void startReconnect();
    void stopReconnect();
    void sendRejoinRequest();
    void abortReconnectAndLeave(const QString &reason,
                                bool keepResumeCredentials = false);
    void clearRoomCredentials();

    NetworkManager *m_networkManager = nullptr;
    RoomSession *m_roomSession = nullptr;

    QString m_clientId;     // 当前连接的 id（welcome 下发；resume 后会被稳定 id 覆盖）
    QString m_roomClientId; // 房间成员表里的稳定 id，重连后不变
    QString m_nickname;

    PendingRequest m_pending = PendingRequest::None;
    QString m_pendingRoomId;
    QString m_pendingRoomName;
    QString m_pendingNickname;
    QString m_pendingPassword;

    bool m_awaitingReply = false;

    // 最近一次成功进入的房间。只放内存，不落盘、不写日志。
    QString m_roomId;
    // 这个房间属于哪台服务器。resume 前校验，避免连到兜底服务器后发 resume。
    QString m_roomServerUrl;
    // 断线重连凭据：稳定成员 id + resume token
    QString m_resumeToken;

    bool m_reconnecting = false;
    bool m_rejoinInFlight = false;
    // 手动“恢复上次会话”请求已发出，等待 room_joined
    bool m_manualResumeInFlight = false;
    int m_rejoinAttempts = 0;       // 仅用于日志
    QTimer m_reconnectDeadline;     // 宽限期：超时后彻底退房
};

#endif //SYNCINE_SESSIONCONTROLLER_H
