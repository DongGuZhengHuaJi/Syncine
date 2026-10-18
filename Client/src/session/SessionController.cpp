//
// Created by donggu on 2026/9/13.
//

#include "SessionController.h"

#include <QRandomGenerator>

#include "core/Log.h"
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

    // 一整轮（用户地址 + 兜底）都未能连上时，将挂起的请求判为失败。
    //
    // 缺少此项会导致死锁：点击「创建房间」后请求挂起、按钮变为禁用的
    // 「连接中…」，而失败路径仅在 onDisconnected 中复位——Qt 在连接被拒时
    // 不一定发出该信号（各版本行为不一致），于是按钮永久禁用，只能重启客户端。
    connect(m_networkManager, &NetworkManager::connectionFailed, this,
            [this](const QString &reason) {
                // 仅在「正等待连接以发送请求」时报错；后台重试的失败不应提示用户
                if (m_pending != PendingRequest::None) {
                    LOG_WARN("Session") << "连接失败，挂起的请求判失败:" << reason;
                    failPending(QStringLiteral("无法连接到服务器"));
                }
            });
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
        LOG_WARN("Session") << "创建房间被拒：已在房间中";
        emit errorOccurred("已在房间中");
        return;
    }

    if (busy()) {
        LOG_WARN("Session") << "创建房间被拒：已有请求正在处理";
        emit errorOccurred("正在连接服务器，请稍候");
        return;
    }

    m_nickname = nickname;
    emit nicknameChanged();

    m_pending = PendingRequest::CreateRoom;
    m_pendingRoomId = generateRoomId();
    m_pendingRoomName = roomName;
    m_pendingNickname = nickname;
    m_pendingPassword = password;

    LOG_INFO("Session") << "请求创建房间: roomId=" << m_pendingRoomId
                        << " nickname=" << nickname;
    emit busyChanged();
    sendPendingRequest();
}

void SessionController::joinRoom(const QString &roomId,
                                 const QString &nickname,
                                 const QString &password) {
    if (m_roomSession != nullptr && m_roomSession->inRoom()) {
        LOG_WARN("Session") << "加入房间被拒：已在房间中";
        emit errorOccurred("已在房间中");
        return;
    }

    if (busy()) {
        LOG_WARN("Session") << "加入房间被拒：已有请求正在处理";
        emit errorOccurred("正在连接服务器，请稍候");
        return;
    }

    m_nickname = nickname;
    emit nicknameChanged();

    m_pending = PendingRequest::JoinRoom;
    m_pendingRoomId = roomId;
    m_pendingNickname = nickname;
    m_pendingPassword = password;

    LOG_INFO("Session") << "请求加入房间: roomId=" << roomId << " nickname=" << nickname;
    emit busyChanged();
    sendPendingRequest();
}

void SessionController::leaveRoom() {
    if (m_roomSession == nullptr || !m_roomSession->inRoom())
        return;

    LOG_INFO("Session") << "主动离开房间: roomId=" << m_roomSession->roomId();

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

    // 尚未连接服务器：先建立连接，连上后 onConnected 会再次调用此处
    if (!m_networkManager->isConnected()) {
        LOG_DEBUG("Session") << "尚未连接服务器，先发起连接";
        m_networkManager->connectToServer();
        return;
    }

    const QString text = m_pending == PendingRequest::CreateRoom
        ? Protocol::encodeCreateRoom(m_pendingRoomId, m_pendingRoomName,
                                     m_pendingNickname, m_pendingPassword)
        : Protocol::encodeJoinRoom(m_pendingRoomId, m_pendingNickname,
                                   m_pendingPassword);

    if (!m_networkManager->sendMessage(text)) {
        LOG_ERROR("Session") << "发送入场请求失败: roomId=" << m_pendingRoomId;
        failPending("无法向服务器发送请求");
        return;
    }

    LOG_DEBUG("Session") << "入场请求已发出: roomId=" << m_pendingRoomId
                         << " 类型=" << (m_pending == PendingRequest::CreateRoom
                                             ? "create_room" : "join_room");
    m_pending = PendingRequest::None;
    setAwaitingReply(true);
}

void SessionController::failPending(const QString &errorString) {
    LOG_WARN("Session") << "入场请求失败:" << errorString;
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
    LOG_DEBUG("Session") << "已连接服务器";
    sendPendingRequest();
}

void SessionController::onDisconnected() {
    LOG_INFO("Session") << "与服务器的连接断开";

    if (m_pending != PendingRequest::None)
        failPending("无法连接到服务器");
    else if (m_awaitingReply)
        setAwaitingReply(false);

    if (m_roomSession != nullptr && m_roomSession->inRoom()) {
        LOG_WARN("Session") << "连接断开时仍在房间内，强制退房: roomId="
                            << m_roomSession->roomId();
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
        LOG_WARN("Session") << "收到无法解析的消息";
        emit errorOccurred("收到无法解析的消息");
        return;
    }

    switch (decoded->type) {
    case Protocol::MessageType::Welcome:
        m_clientId = decoded->clientId;
        LOG_INFO("Session") << "收到 welcome: clientId=" << m_clientId;
        break;

    case Protocol::MessageType::RoomCreated:
    case Protocol::MessageType::RoomJoined:
        handleRoomEntered(*decoded);
        break;

    case Protocol::MessageType::RoomLeft:
        LOG_INFO("Session") << "服务端通知房间已关闭";
        if (m_roomSession != nullptr)
            m_roomSession->leaveRoom();
        break;

    case Protocol::MessageType::Error:
        LOG_WARN("Session") << "服务端返回错误: code=" << decoded->code
                            << " message=" << decoded->errorMessage;
        if (m_awaitingReply)
            setAwaitingReply(false);

        emit errorOccurred(decoded->errorMessage.isEmpty() ? decoded->code
                                                           : decoded->errorMessage);
        break;

    default:
        // 房间内消息与 WebRTC 信令由 RoomSession / SignalingChannel 各自订阅，
        // 此处不转发
        break;
    }
}

void SessionController::handleRoomEntered(const Protocol::Message &message) {
    const RoomSnapshot snapshot = snapshotFrom(message);

    m_pending = PendingRequest::None;
    setAwaitingReply(false);

    // 顺序有要求：先推起房间页（QML 在 Connections 中接收房间信号），
    // 再灌入房间状态。顺序颠倒会导致首次的 videoMismatch 等信号在页面
    // 尚不存在时发出而被静默丢弃。
    if (message.type == Protocol::MessageType::RoomCreated)
        emit roomCreated();
    else
        emit roomJoined();

    LOG_INFO("Session") << "已进入房间: roomId=" << snapshot.roomId
                        << " name=" << snapshot.roomName
                        << " mode=" << snapshot.mode
                        << " 成员数=" << snapshot.members.size()
                        << " 播放列表=" << snapshot.playlist.size();

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
    snapshot.rate = message.rate;
    snapshot.videoMismatched = message.videoMismatched;
    snapshot.shortestDuration = message.shortestDuration;
    // 播放列表随 state 一并下发，漏掉这两行会导致入房者看到空列表
    snapshot.playlist = message.playlist;
    snapshot.currentIndex = message.currentIndex;
    // ICE 配置（STUN/TURN）同样随入房消息到达。SignalingChannel 建连前会取用它，
    // 因此必须进入 snapshot，时序说明见 RoomSession::enterRoom。
    snapshot.iceServers = message.iceServers;
    return snapshot;
}
