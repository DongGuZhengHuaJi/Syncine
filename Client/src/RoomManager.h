//
// Created by donggu on 2026/9/7.
//

#ifndef SYNCINE_ROOMMANAGER_H
#define SYNCINE_ROOMMANAGER_H

#include <QObject>
#include <qqmlintegration.h>
#include <QUrl>
#include <QVariant>

#include "PlaybackController.h"

class NetworkManager;
class QJsonArray;
class QJsonObject;



class RoomManager : public QObject {
    Q_OBJECT
    QML_ELEMENT

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

    Q_PROPERTY(RoomMode roomMode
               READ roomMode
               NOTIFY roomModeChanged)

    Q_PROPERTY(bool otherLoaded
               READ otherLoaded
               NOTIFY otherLoadedChanged)

    Q_PROPERTY(bool allLoaded
               READ allLoaded
               NOTIFY allLoadedChanged)

    Q_PROPERTY(bool videoMismatched
               READ videoMismatched
               NOTIFY videoMismatchChanged)

    Q_PROPERTY(qint64 shortestDuration
               READ shortestDuration
               NOTIFY videoMismatchChanged)

public:

    enum class RoomMode {
        Local,
        Share,
        Url
    };
    Q_ENUM(RoomMode)

    explicit RoomManager(QObject *parent = nullptr);
    ~RoomManager() override = default;

    void setNetworkManager(NetworkManager *networkManager);
    void setPlaybackController(PlaybackController *playbackController);


    QString roomId() const;
    QString roomName() const;
    RoomMode roomMode() const;
    bool otherLoaded() const;
    bool allLoaded() const;
    bool videoMismatched() const;
    qint64 shortestDuration() const;
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

    Q_INVOKABLE void setRoomMode(RoomMode mode);

    Q_INVOKABLE void sendChat(const QString &text);
    Q_INVOKABLE void sendPlayback(const QString &action, qint64 position);

    // WebRTC 信令转发(经由服务器定向发给同房间的指定成员)
    Q_INVOKABLE void sendWebrtcOffer(const QString &to, const QString &sdp);
    Q_INVOKABLE void sendWebrtcAnswer(const QString &to, const QString &sdp);
    Q_INVOKABLE void sendWebrtcIce(const QString &to, const QString &sdp,
                                   const QString &sdpMid, int sdpMLineIndex);

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
    void roomModeChanged();
    void otherLoadedChanged();
    void allLoadedChanged();
    void videoMismatchChanged();

    void memberJoined(const QString &nickname);
    void memberLeft(const QString &nickname);

    void chatReceived(const QString &from, const QString &text);
    void playbackReceived(const QString &action, qint64 position);

    void webrtcOfferReceived(const QString &from, const QString &sdp);
    void webrtcAnswerReceived(const QString &from, const QString &sdp);
    void webrtcIceReceived(const QString &from, const QString &sdp,
                           const QString &sdpMid, int sdpMLineIndex);

    void errorOccurred(const QString &errorString);

private slots:
    void handleMessage(const QString &message);
    void onNetworkConnected();
    void onNetworkDisconnected();

    void onPlaybackSourceChanged();

private:
    enum class PendingAction { None, CreateRoom, JoinRoom };

    void send(const QJsonObject &message);
    void sendPendingRequest();
    void failPending(const QString &error);
    void setAwaitingReply(bool awaiting);
    void applyMembers(const QJsonArray &membersArray);
    void clearRoomState();

    NetworkManager *m_networkManager = nullptr;
    PlaybackController *m_playbackController = nullptr;

    QString m_roomId;
    QString m_roomName;
    bool m_inRoom = false;
    RoomMode m_roomMode = RoomMode::Local;
    bool m_videoMismatched = false;
    qint64 m_shortestDuration = 0;
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
