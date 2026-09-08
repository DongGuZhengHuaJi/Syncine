//
// Created by donggu on 2026/9/7.
//

#ifndef SYNCINE_ROOMMANAGER_H
#define SYNCINE_ROOMMANAGER_H

#include <QObject>
#include <QUrl>
#include <QVariant>

class NetworkManager;
class QJsonArray;
class QJsonObject;

class RoomManager : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString roomId
               READ roomId
               NOTIFY roomIdChanged)

    Q_PROPERTY(QString roomName
               READ roomName
               NOTIFY roomNameChanged)

    Q_PROPERTY(bool inRoom
               READ inRoom
               NOTIFY inRoomChanged)

    Q_PROPERTY(bool isConnecting
               READ isConnecting
               NOTIFY isConnectingChanged)

    Q_PROPERTY(QString nickname
               READ nickname
               NOTIFY nicknameChanged)

    Q_PROPERTY(QString clientId
               READ clientId
               NOTIFY clientIdChanged)

    Q_PROPERTY(bool isHost
               READ isHost
               NOTIFY isHostChanged)

    Q_PROPERTY(QVariantList members
               READ members
               NOTIFY membersChanged)

public:
    explicit RoomManager(QObject *parent = nullptr);
    ~RoomManager() override = default;

    // 由 main.cpp 调用一次,把网络模块注入进来
    void setNetworkManager(NetworkManager *networkManager);

    QString roomId() const;
    QString roomName() const;
    bool inRoom() const;
    bool isConnecting() const;
    QString nickname() const;
    QString clientId() const;
    bool isHost() const;
    QVariantList members() const;

    Q_INVOKABLE void createRoom(const QString &nickname,
                                const QString &password,
                                const QString &roomName);
    Q_INVOKABLE void joinRoom(const QString &roomId,
                              const QString &nickname,
                              const QString &password);
    Q_INVOKABLE void leaveRoom();

    Q_INVOKABLE void sendChat(const QString &text);
    Q_INVOKABLE void sendPlayback(const QString &action, qint64 position);

signals:
    void roomIdChanged();
    void roomNameChanged();
    void inRoomChanged();
    void isConnectingChanged();
    void nicknameChanged();
    void clientIdChanged();
    void isHostChanged();
    void membersChanged();
    void serverUrlChanged();

    void roomCreated();
    void roomJoined();
    void roomLeft();

    void memberJoined(const QString &nickname);
    void memberLeft(const QString &nickname);

    void chatReceived(const QString &from, const QString &text);
    void playbackReceived(const QString &action, qint64 position);

    void errorOccurred(const QString &errorString);

private slots:
    void handleMessage(const QString &message);
    void onNetworkConnected();
    void onNetworkDisconnected();

private:
    enum class PendingAction { None, CreateRoom, JoinRoom };

    void send(const QJsonObject &message);
    void sendPendingRequest();
    void failPending(const QString &error);
    void setAwaitingReply(bool awaiting);
    void applyMembers(const QJsonArray &membersArray);
    void clearRoomState();

    NetworkManager *m_networkManager = nullptr;

    QString m_roomId;
    QString m_roomName;
    bool m_inRoom = false;
    QString m_nickname;
    QString m_clientId;
    QVariantList m_members;

    PendingAction m_pendingAction = PendingAction::None;
    QString m_pendingRoomId;
    QString m_pendingNickname;
    QString m_pendingPassword;
    QString m_pendingRoomName;

    bool m_awaitingReply = false;
};


#endif //SYNCINE_ROOMMANAGER_H
