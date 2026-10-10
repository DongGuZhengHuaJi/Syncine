//
// Created by donggu on 2026/9/13.
//

#include "SessionController.h"

#include <QRandomGenerator>

#include "core/NetworkManager.h"
#include "session/RoomSession.h"

namespace {

constexpr int kRoomIdMax = 1000000; // 房间号范围:0 ~ 999999

QString generateRoomId() {
    return QString::number(QRandomGenerator::global()->bounded(kRoomIdMax))
        .rightJustified(6, '0');
}

} // namespace

SessionController::SessionController(NetworkManager *networkManager,
                                     RoomSession *roomSession,
                                     QObject *parent)
    : QObject(parent),
      m_networkManager(networkManager),
      m_roomSession(roomSession) {

    if (m_networkManager == nullptr)
        return;

    connect(m_networkManager, &NetworkManager::messageReceived,
            this, &SessionController::onMessage);
    connect(m_networkManager, &NetworkManager::connected,
            this, &SessionController::onConnected);
    connect(m_networkManager, &NetworkManager::disconnected,
            this, &SessionController::onDisconnected);
}

bool SessionController::busy() const {
    return m_pending != PendingRequest::None || m_awaitingReply;
}

QString SessionController::nickname() const {
    return m_nickname;
}

// ============================
// 入场请求
// ============================

void SessionController::createRoom(const QString &roomName,
                                   const QString &nickname,
                                   const QString &password) {
    if (m_roomSession != nullptr && m_roomSession->inRoom()) {
        emit errorOccurred("你已经在房间里了");
        return;
    }

    if (busy()) {
        emit errorOccurred("正在连接服务器,请稍候");
        return;
    }

    m_nickname = nickname;
    emit nicknameChanged();

    m_pending = PendingRequest::CreateRoom;
    m_pendingRoomId = generateRoomId();
    m_pendingRoomName = roomName;
    m_pendingNickname = nickname;
    m_pendingPassword = password;

    emit busyChanged();
    sendPendingRequest();
}

void SessionController::joinRoom(const QString &roomId,
                                 const QString &nickname,
                                 const QString &password) {
    if (m_roomSession != nullptr && m_roomSession->inRoom()) {
        emit errorOccurred("你已经在房间里了");
        return;
    }

    if (busy()) {
        emit errorOccurred("正在连接服务器,请稍候");
        return;
    }

    m_nickname = nickname;
    emit nicknameChanged();

    m_pending = PendingRequest::JoinRoom;
    m_pendingRoomId = roomId;
    m_pendingNickname = nickname;
    m_pendingPassword = password;

    emit busyChanged();
    sendPendingRequest();
}

void SessionController::leaveRoom() {
    if (m_roomSession == nullptr || !m_roomSession->inRoom())
        return;

    if (m_networkManager != nullptr && m_networkManager->isConnected())
        m_networkManager->sendMessage(Protocol::encodeLeaveRoom());

    m_roomSession->leaveRoom();
}

void SessionController::sendPendingRequest() {
    if (m_pending == PendingRequest::None)
        return;

    if (m_networkManager == nullptr) {
        failPending("网络模块未初始化");
        return;
    }

    // 还没连上服务器:先建立连接,连上之后 onConnected 会再调一次这里
    if (!m_networkManager->isConnected()) {
        m_networkManager->connectToServer();
        return;
    }

    const QString text = m_pending == PendingRequest::CreateRoom
        ? Protocol::encodeCreateRoom(m_pendingRoomId, m_pendingRoomName,
                                     m_pendingNickname, m_pendingPassword)
        : Protocol::encodeJoinRoom(m_pendingRoomId, m_pendingNickname,
                                   m_pendingPassword);

    if (!m_networkManager->sendMessage(text)) {
        failPending("无法向服务器发送请求");
        return;
    }

    m_pending = PendingRequest::None;
    setAwaitingReply(true);
}

void SessionController::failPending(const QString &errorString) {
    m_pending = PendingRequest::None;
    m_awaitingReply = false;
    emit busyChanged();
    emit errorOccurred(errorString);
}

void SessionController::setAwaitingReply(bool awaiting) {
    if (m_awaitingReply == awaiting)
        return;

    m_awaitingReply = awaiting;
    emit busyChanged();
}

// ============================
// 连接状态
// ============================

void SessionController::onConnected() {
    sendPendingRequest();
}

void SessionController::onDisconnected() {
    if (m_pending != PendingRequest::None)
        failPending("无法连接到服务器");
    else if (m_awaitingReply)
        setAwaitingReply(false);

    if (m_roomSession != nullptr && m_roomSession->inRoom()) {
        m_roomSession->leaveRoom();
        emit errorOccurred("与服务器的连接已断开");
    }
}

// ============================
// 消息分发
// ============================

void SessionController::onMessage(const QString &text) {
    const std::optional<Protocol::Message> decoded = Protocol::decode(text);
    if (!decoded) {
        emit errorOccurred("收到无法解析的消息");
        return;
    }

    switch (decoded->type) {
    case Protocol::MessageType::Welcome:
        m_clientId = decoded->clientId;
        break;

    case Protocol::MessageType::RoomCreated:
    case Protocol::MessageType::RoomJoined:
        handleRoomEntered(*decoded);
        break;

    case Protocol::MessageType::RoomLeft:
        if (m_roomSession != nullptr)
            m_roomSession->leaveRoom();
        break;

    case Protocol::MessageType::Error:
        if (m_awaitingReply)
            setAwaitingReply(false);

        emit errorOccurred(decoded->errorMessage.isEmpty() ? decoded->code
                                                           : decoded->errorMessage);
        break;

    default:
        // 房间内消息和 WebRTC 信令由 RoomSession / SignalingChannel 各自订阅,
        // 这里不做转发
        break;
    }
}

void SessionController::handleRoomEntered(const Protocol::Message &message) {
    const RoomSnapshot snapshot = snapshotFrom(message);

    m_pending = PendingRequest::None;
    setAwaitingReply(false);

    // 顺序很重要:先把房间页推起来(QML 在 Connections 里接房间信号),
    // 再把房间状态灌进去。反过来的话,首次的 videoMismatch 之类的信号
    // 会在页面还不存在时发出去,被静默丢掉。
    if (message.type == Protocol::MessageType::RoomCreated)
        emit roomCreated();
    else
        emit roomJoined();

    if (m_roomSession != nullptr)
        m_roomSession->enterRoom(snapshot);
}

RoomSnapshot SessionController::snapshotFrom(const Protocol::Message &message) const {
    RoomSnapshot snapshot;
    snapshot.roomId = message.roomId;
    snapshot.roomName = message.roomName;
    snapshot.mode = message.mode;
    snapshot.clientId = m_clientId;
    snapshot.members = message.members;
    snapshot.hasState = message.hasState;
    snapshot.playing = message.playing;
    snapshot.position = message.position;
    snapshot.videoMismatched = message.videoMismatched;
    snapshot.shortestDuration = message.shortestDuration;
    // 播放列表随 state 一起下发 —— 漏了这两行,入房的人会看到一个空列表
    snapshot.playlist = message.playlist;
    snapshot.currentIndex = message.currentIndex;
    return snapshot;
}
