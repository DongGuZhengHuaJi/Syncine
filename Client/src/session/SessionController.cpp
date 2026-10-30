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

// 信令断开后保留房间、等待自动回房的总时长。
// NetworkManager 的重试节奏是 15s → 30s → 60s；服务端的 ping 检测半开连接
// 也需要几十秒，所以留 90 秒，覆盖"先重连、再等服务端释放旧成员"这两段。
constexpr int kReconnectGraceMs = 90000;

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

    // 进/退房会改变“是否还能恢复上次会话”，直接转发给 QML
    if (m_roomSession != nullptr) {
        connect(m_roomSession, &RoomSession::inRoomChanged,
                this, &SessionController::canResumeLastSessionChanged);
    }

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

    // 宽限期结束还没回房：彻底退出，避免房间页永远停在"重连中"。
    m_reconnectDeadline.setSingleShot(true);
    connect(&m_reconnectDeadline, &QTimer::timeout, this, [this] {
        if (!m_reconnecting)
            return;
        LOG_WARN("Session") << "重连宽限期结束,退出房间:" << m_roomId;
        abortReconnectAndLeave(
            QStringLiteral("与服务器的连接已断开，可以在加入房间页恢复上次会话"),
            /*keepResumeCredentials=*/true);
    });
}

bool SessionController::busy() const {
    return m_pending != PendingRequest::None || m_awaitingReply;
}

QString SessionController::nickname() const {
    return m_nickname;
}

bool SessionController::canResumeLastSession() const {
    return m_roomSession != nullptr
           && !m_roomSession->inRoom()
           && !m_reconnecting
           && !m_roomId.isEmpty()
           && !m_roomClientId.isEmpty()
           && !m_resumeToken.isEmpty();
}

QString SessionController::resumeRoomId() const {
    return canResumeLastSession() ? m_roomId : QString();
}

void SessionController::resumeLastSession() {
    if (m_roomSession != nullptr && m_roomSession->inRoom()) {
        emit errorOccurred("已在房间中");
        return;
    }
    if (m_reconnecting) {
        emit errorOccurred("正在自动重连，请稍候");
        return;
    }
    if (!canResumeLastSession()) {
        emit errorOccurred("没有可恢复的会话");
        return;
    }
    if (busy()) {
        emit errorOccurred("正在处理其他请求，请稍候");
        return;
    }

    LOG_INFO("Session") << "手动恢复上次会话: roomId=" << m_roomId
                        << " clientId=" << m_roomClientId;
    m_manualResumeInFlight = true;
    m_pending = PendingRequest::ResumeLastRoom;
    m_pendingRoomId = m_roomId;
    m_pendingNickname = m_nickname;
    m_pendingPassword.clear();
    emit busyChanged();
    sendPendingRequest();
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

    if (m_reconnecting) {
        LOG_WARN("Session") << "创建房间被拒：正在重连房间";
        emit errorOccurred("正在重连房间，请稍候");
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

    if (m_reconnecting) {
        LOG_WARN("Session") << "加入房间被拒：正在重连房间";
        emit errorOccurred("正在重连房间，请稍候");
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
    // 用户主动离开：宽限期/重试都作废，不管当前是否还在房间。
    stopReconnect();

    if (m_roomSession == nullptr || !m_roomSession->inRoom()) {
        clearRoomCredentials();
        return;
    }

    LOG_INFO("Session") << "主动离开房间: roomId=" << m_roomSession->roomId();

    // 断线重连期间用户主动离开：网络可能还没恢复，发不出去就不发。
    if (m_networkManager != nullptr && m_networkManager->isConnected())
        m_networkManager->sendMessage(Protocol::encodeLeaveRoom());

    m_roomSession->leaveRoom();
    clearRoomCredentials();
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

    const bool isManualResume = (m_pending == PendingRequest::ResumeLastRoom);
    QString text;
    if (m_pending == PendingRequest::CreateRoom) {
        text = Protocol::encodeCreateRoom(m_pendingRoomId, m_pendingRoomName,
                                          m_pendingNickname, m_pendingPassword);
    } else if (isManualResume) {
        if (m_roomClientId.isEmpty() || m_resumeToken.isEmpty()) {
            failPending(QStringLiteral("会话凭据已失效"));
            return;
        }
        // 房间只存在于原来的服务器：连到别的地址时不能发 resume
        if (!m_roomServerUrl.isEmpty()
            && m_networkManager->effectiveUrl() != m_roomServerUrl) {
            failPending(QStringLiteral("当前连接的服务器与上次会话不一致"));
            clearRoomCredentials();
            return;
        }
        text = Protocol::encodeResume(m_pendingRoomId, m_roomClientId,
                                      m_resumeToken);
    } else {
        text = Protocol::encodeJoinRoom(m_pendingRoomId, m_pendingNickname,
                                        m_pendingPassword);
    }

    if (!m_networkManager->sendMessage(text)) {
        LOG_ERROR("Session") << "发送入场请求失败: roomId=" << m_pendingRoomId;
        failPending("无法向服务器发送请求");
        return;
    }

    LOG_DEBUG("Session") << "入场请求已发出: roomId=" << m_pendingRoomId
                         << " 类型=" << (m_pending == PendingRequest::CreateRoom
                                             ? "create_room"
                                             : (isManualResume ? "resume" : "join_room"));
    m_manualResumeInFlight = isManualResume;
    m_pending = PendingRequest::None;
    setAwaitingReply(true);
}

void SessionController::failPending(const QString &errorString) {
    LOG_WARN("Session") << "入场请求失败:" << errorString;
    m_pending = PendingRequest::None;
    m_awaitingReply = false;
    m_manualResumeInFlight = false;
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

    // 重连中的优先级最高：先回房，不再处理普通的入场请求。
    if (m_reconnecting) {
        sendRejoinRequest();
        return;
    }

    sendPendingRequest();
}

void SessionController::onDisconnected() {
    LOG_INFO("Session") << "与服务器的连接断开";

    // 断线时正在途中的回房请求一定不会有结果了；等下一次 connected 再发。
    m_rejoinInFlight = false;

    if (m_pending != PendingRequest::None)
        failPending("无法连接到服务器");
    else if (m_awaitingReply)
        setAwaitingReply(false);

    if (m_manualResumeInFlight) {
        m_manualResumeInFlight = false;
        emit errorOccurred("与服务器的连接已断开，请重试恢复");
    }

    if (m_roomSession != nullptr && m_roomSession->inRoom()) {
        // 与信令服务器断开，如果在房间内尝试重新连接
        if (!m_reconnecting)
            startReconnect();
        return;
    }

    // 不在房间，直接断开连接
    stopReconnect();
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
        // 重连期间 welcome 里是新连接的临时 id；稳定的成员 id 由随后
        // 的 room_joined(resumed=true) 给出。这里不要覆盖重连前的值。
        if (!m_reconnecting) {
            m_clientId = decoded->clientId;
            LOG_INFO("Session") << "收到 welcome: clientId=" << m_clientId;
        } else {
            LOG_DEBUG("Session") << "重连 welcome: connectionId=" << decoded->clientId;
        }
        break;

    case Protocol::MessageType::RoomCreated:
    case Protocol::MessageType::RoomJoined:
        handleRoomEntered(*decoded);
        break;

    case Protocol::MessageType::RoomLeft:
        LOG_INFO("Session") << "服务端通知房间已关闭";
        if (m_reconnecting) {
            abortReconnectAndLeave(QStringLiteral("房间已解散"));
            return;
        }
        if (m_roomSession != nullptr)
            m_roomSession->leaveRoom();
        clearRoomCredentials();
        break;

    case Protocol::MessageType::Error:
        LOG_WARN("Session") << "服务端返回错误: code=" << decoded->code
                            << " message=" << decoded->errorMessage;

        // 房间内 WebRTC 信令的竞态错误：目标成员刚好离线/离开，或者消息
        // 晚于 leave_room。不是用户操作失败，只记日志，不弹窗，也不影响
        // 正在等待中的 resume/join。
        if (decoded->code == QLatin1String("member_not_found")
            || decoded->code == QLatin1String("not_in_room")) {
            LOG_DEBUG("Session") << "忽略房间内的信令竞态错误:" << decoded->code;
            return;
        }

        if (m_awaitingReply)
            setAwaitingReply(false);

        // 手动“恢复上次会话”的错误：单独处理，不走自动重连分支
        if (m_manualResumeInFlight) {
            m_manualResumeInFlight = false;
            const bool sessionGone =
                decoded->code == QLatin1String("resume_failed")
                || decoded->code == QLatin1String("room_not_found")
                || decoded->code == QLatin1String("bad_message");
            if (sessionGone)
                clearRoomCredentials();

            const QString text =
                decoded->code == QLatin1String("resume_failed")
                    ? QStringLiteral("上次会话已过期，请重新加入房间")
                    : (decoded->errorMessage.isEmpty() ? decoded->code
                                                       : decoded->errorMessage);
            emit errorOccurred(text);
            return;
        }

        if (m_reconnecting && m_rejoinInFlight) {
            m_rejoinInFlight = false;

            if (decoded->code == QLatin1String("resume_failed")) {
                abortReconnectAndLeave(QStringLiteral("会话已过期，请重新进入房间"));
                return;
            }

            // room_not_found / wrong_password 等都没法靠重试恢复
            abortReconnectAndLeave(decoded->errorMessage.isEmpty()
                                       ? decoded->code
                                       : decoded->errorMessage);
            return;
        }

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
    const bool wasReconnecting = m_reconnecting;

    // 服务端下发稳定成员 id 和 resume token；resume 后 clientId 仍是断线前的值。
    m_clientId = message.clientId;
    m_roomClientId = message.clientId;
    m_resumeToken = message.resumeToken;
    if (m_networkManager != nullptr)
        m_roomServerUrl = m_networkManager->effectiveUrl();

    const RoomSnapshot snapshot = snapshotFrom(message);

    m_pending = PendingRequest::None;
    setAwaitingReply(false);
    m_rejoinInFlight = false;
    m_manualResumeInFlight = false;

    if (wasReconnecting) {
        // 重连成功：房间页已经存在，不能再发 roomJoined 让它 push 一次。
        stopReconnect();
        m_rejoinAttempts = 0;
        LOG_INFO("Session") << "重连成功，恢复房间:" << message.roomId
                            << " clientId=" << m_roomClientId
                            << (message.resumed ? " (resume)" : " (join)");
    } else {
        // 记录本次入房凭据，供断线重连使用（口令不保存）。
        m_roomId = message.roomId;
        m_rejoinAttempts = 0;

        // 顺序有要求：先推起房间页（QML 在 Connections 中接收房间信号），
        // 再灌入房间状态。顺序颠倒会导致首次的 videoMismatch 等信号在页面
        // 尚不存在时发出而被静默丢弃。
        if (message.type == Protocol::MessageType::RoomCreated)
            emit roomCreated();
        else
            emit roomJoined();
    }

    LOG_INFO("Session") << "已进入房间: roomId=" << snapshot.roomId
                        << " name=" << snapshot.roomName
                        << " mode=" << snapshot.mode
                        << " 成员数=" << snapshot.members.size()
                        << " 播放列表=" << snapshot.playlist.size()
                        << (message.resumed ? " (resume)"
                                            : (wasReconnecting ? " (重连)" : ""));

    if (m_roomSession != nullptr)
        m_roomSession->enterRoom(snapshot);

    if (wasReconnecting)
        emit roomResumed();
}

// ============================
// 断线重连
// ============================

void SessionController::startReconnect() {
    if (m_reconnecting)
        return;

    m_reconnecting = true;
    m_rejoinInFlight = false;
    m_rejoinAttempts = 0;
    m_reconnectDeadline.start(kReconnectGraceMs);

    if (m_roomId.isEmpty() && m_roomSession != nullptr)
        m_roomId = m_roomSession->roomId();

    LOG_WARN("Session") << "信令断开，保留房间并等待重连:" << m_roomId;
    emit reconnectingChanged();
    emit canResumeLastSessionChanged();
}

void SessionController::stopReconnect() {
    m_reconnectDeadline.stop();

    if (!m_reconnecting)
        return;

    m_reconnecting = false;
    m_rejoinInFlight = false;
    emit reconnectingChanged();
    emit canResumeLastSessionChanged();
}

void SessionController::sendRejoinRequest() {
    if (!m_reconnecting || m_rejoinInFlight || m_roomId.isEmpty())
        return;
    if (m_networkManager == nullptr || !m_networkManager->isConnected())
        return;

    // 只走 resume：没有 token 说明入房时服务端没下发，属于协议异常，
    if (m_resumeToken.isEmpty() || m_roomClientId.isEmpty()) {
        abortReconnectAndLeave(QStringLiteral("缺少会话恢复凭据"));
        return;
    }

    // 防兜底地址：房间只存在于原服务器，连到别的地址时 resume 没有意义
    if (!m_roomServerUrl.isEmpty()
        && m_networkManager->effectiveUrl() != m_roomServerUrl) {
        abortReconnectAndLeave(QStringLiteral("当前连接的服务器与上次会话不一致"));
        return;
    }

    const QString text =
        Protocol::encodeResume(m_roomId, m_roomClientId, m_resumeToken);
    if (!m_networkManager->sendMessage(text)) {
        // 连接又发生中断
        LOG_WARN("Session") << "重连请求发送失败，等待下一次连接";
        return;
    }

    m_rejoinInFlight = true;
    ++m_rejoinAttempts;
    LOG_INFO("Session") << "重连：恢复会话" << m_roomId
                        << " clientId=" << m_roomClientId
                        << " (第" << m_rejoinAttempts << "次)";
}

void SessionController::abortReconnectAndLeave(const QString &reason,
                                               bool keepResumeCredentials) {
    LOG_WARN("Session") << "重连失败，退出房间:" << reason;

    stopReconnect();
    m_rejoinInFlight = false;
    m_manualResumeInFlight = false;

    if (m_roomSession != nullptr && m_roomSession->inRoom())
        m_roomSession->leaveRoom();

    if (keepResumeCredentials) {
        // 保留 roomId / 稳定 clientId / token：用户可以到加入房间页手动 resume
        emit canResumeLastSessionChanged();
    } else {
        clearRoomCredentials();
    }
    emit errorOccurred(reason);
}

void SessionController::clearRoomCredentials() {
    m_roomId.clear();
    m_roomServerUrl.clear();
    m_roomClientId.clear();
    m_resumeToken.clear();
    m_rejoinAttempts = 0;
    m_manualResumeInFlight = false;
    emit canResumeLastSessionChanged();
}

RoomSnapshot SessionController::snapshotFrom(const Protocol::Message &message) const {
    RoomSnapshot snapshot;
    snapshot.roomId = message.roomId;
    snapshot.roomName = message.roomName;
    snapshot.mode = message.mode;
    snapshot.shareSource = message.shareSource;
    snapshot.shareAudio = message.shareAudio;
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
