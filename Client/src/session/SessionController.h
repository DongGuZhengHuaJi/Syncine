//
// 会话控制器 —— 只负责「进入房间的过程」,不负责「房间里的状态」。
//
// 为什么要和 RoomSession 分开:
//   建连、等待应答、失败重试都是**瞬态过程**,房间成员表是**稳态状态**。
//   两者混在一个类里,就会出现一个 isConnecting 同时表示
//   "正在连服务器"和"正在等房间应答"两种含义的情况。
//
// 它负责:
//   * WebSocket 的连接/断开时机(必要时自动发起连接)
//   * create_room / join_room / leave_room 的请求状态机
//   * welcome / room_created / room_joined / room_left / error 这几类消息
//
// 因为管的是「过程」,入场成功后它就把结果(RoomSnapshot)交给 RoomSession,
// 自己的待发状态立刻清空 —— 之后房间怎么变,它一概不关心。
//

#ifndef SYNCINE_SESSIONCONTROLLER_H
#define SYNCINE_SESSIONCONTROLLER_H

#include <QObject>
#include <QString>

#include <qqmlintegration.h>

#include "core/Protocol.h"
#include "core/RoomTypes.h"

class NetworkManager;
class RoomSession;

class SessionController : public QObject {
    Q_OBJECT
    QML_ELEMENT

    // 有请求在飞(建连中或等待服务端应答),界面据此禁用按钮
    Q_PROPERTY(bool busy
               READ busy
               NOTIFY busyChanged)

    Q_PROPERTY(QString nickname
               READ nickname
               NOTIFY nicknameChanged)

public:
    SessionController(NetworkManager *networkManager,
                      RoomSession *roomSession,
                      QObject *parent = nullptr);
    ~SessionController() override = default;

    bool busy() const;
    QString nickname() const;

    Q_INVOKABLE void createRoom(const QString &roomName,
                                const QString &nickname,
                                const QString &password);
    Q_INVOKABLE void joinRoom(const QString &roomId,
                              const QString &nickname,
                              const QString &password);
    Q_INVOKABLE void leaveRoom();

signals:
    void busyChanged();
    void nicknameChanged();

    void roomCreated();
    void roomJoined();

    void errorOccurred(const QString &errorString);

private slots:
    void onMessage(const QString &text);
    void onConnected();
    void onDisconnected();

private:
    enum class PendingRequest {
        None,
        CreateRoom,
        JoinRoom
    };

    void handleRoomEntered(const Protocol::Message &message);
    void sendPendingRequest();
    void failPending(const QString &errorString);
    void setAwaitingReply(bool awaiting);
    RoomSnapshot snapshotFrom(const Protocol::Message &message) const;

    NetworkManager *m_networkManager = nullptr;
    RoomSession *m_roomSession = nullptr;

    QString m_clientId;     // welcome 握手拿到的身份
    QString m_nickname;

    PendingRequest m_pending = PendingRequest::None;
    QString m_pendingRoomId;
    QString m_pendingRoomName;
    QString m_pendingNickname;
    QString m_pendingPassword;

    bool m_awaitingReply = false;
};

#endif //SYNCINE_SESSIONCONTROLLER_H
