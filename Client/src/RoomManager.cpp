//
// Created by donggu on 2026/9/7.
//

#include "RoomManager.h"
#include "NetworkManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTimer>

namespace {

constexpr int kRoomIdMax = 1000000; // 房间号范围:0 ~ 999999

QString generateRoomId() {
    return QString::number(QRandomGenerator::global()->bounded(kRoomIdMax))
        .rightJustified(6, '0');
}

RoomManager::RoomMode modeFromString(const QString &mode) {
    if (mode == "share")
        return RoomManager::RoomMode::Share;
    if (mode == "url")
        return RoomManager::RoomMode::Url;
    return RoomManager::RoomMode::Local;
}

} // namespace

RoomManager::RoomManager(QObject *parent)
    : QObject(parent) {
}

void RoomManager::setNetworkManager(NetworkManager *networkManager) {
    if (m_networkManager == networkManager)
        return;

    m_networkManager = networkManager;

    if (m_networkManager == nullptr)
        return;

    connect(m_networkManager, &NetworkManager::messageReceived,
            this, &RoomManager::handleMessage);
    connect(m_networkManager, &NetworkManager::connected,
            this, &RoomManager::onNetworkConnected);
    connect(m_networkManager, &NetworkManager::disconnected,
            this, &RoomManager::onNetworkDisconnected);
}

void RoomManager::setPlaybackController(PlaybackController *playbackController) {
    if (m_playbackController == playbackController)
        return;

    m_playbackController = playbackController;

    if (m_playbackController == nullptr)
        return;

    connect(m_playbackController, &PlaybackController::sourceChanged,
            this, &RoomManager::onPlaybackSourceChanged);
    // 视频时长在元数据加载完成后才知道,到时再补报一次
    connect(m_playbackController, &PlaybackController::durationChanged,
            this, &RoomManager::onPlaybackSourceChanged);
}



// ============================
// 属性
// ============================

QString RoomManager::roomId() const {
    return m_roomId;
}

QString RoomManager::roomName() const {
    return m_roomName;
}

RoomManager::RoomMode RoomManager::roomMode() const {
    return m_roomMode;
}

bool RoomManager::otherLoaded() const {
    for (const QVariant &value : m_members) {
        const QVariantMap member = value.toMap();
        if (member.value("clientId").toString() == m_clientId)
            continue;
        if (!member.value("loaded").toBool())
            return false;
    }
    return true;
}

bool RoomManager::allLoaded() const {
    if (m_playbackController != nullptr && !m_playbackController->hasLoaded())
        return false;
    return otherLoaded();
}

bool RoomManager::videoMismatched() const {
    return m_videoMismatched;
}

qint64 RoomManager::shortestDuration() const {
    return m_shortestDuration;
}

bool RoomManager::inRoom() const {
    return m_inRoom;
}

bool RoomManager::isConnecting() const {
    return m_pendingAction != PendingAction::None || m_awaitingReply;
}

QString RoomManager::nickname() const {
    return m_nickname;
}

QString RoomManager::clientId() const {
    return m_clientId;
}

bool RoomManager::isHost() const {
    for (const QVariant &value : m_members) {
        const QVariantMap member = value.toMap();
        if (member.value("clientId").toString() == m_clientId)
            return member.value("isHost").toBool();
    }
    return false;
}

QVariantList RoomManager::members() const {
    return m_members;
}


// ============================
// 房间操作
// ============================

void RoomManager::createRoom(const QString &nickname,
                             const QString &password,
                             const QString &roomName) {
    if (m_inRoom) {
        emit errorOccurred("你已经在房间里了");
        return;
    }

    if (isConnecting()) {
        emit errorOccurred("正在连接服务器,请稍候");
        return;
    }

    m_nickname = nickname;
    emit nicknameChanged();

    m_pendingAction = PendingAction::CreateRoom;
    m_pendingRoomId = generateRoomId();
    m_pendingNickname = nickname;
    m_pendingPassword = password;
    m_pendingRoomName = roomName;

    emit isConnectingChanged();
    sendPendingRequest();
}

void RoomManager::joinRoom(const QString &roomId,
                           const QString &nickname,
                           const QString &password) {
    if (m_inRoom) {
        emit errorOccurred("你已经在房间里了");
        return;
    }

    if (isConnecting()) {
        emit errorOccurred("正在连接服务器,请稍候");
        return;
    }

    m_nickname = nickname;
    emit nicknameChanged();

    m_pendingAction = PendingAction::JoinRoom;
    m_pendingRoomId = roomId;
    m_pendingNickname = nickname;
    m_pendingPassword = password;

    emit isConnectingChanged();
    sendPendingRequest();
}

void RoomManager::leaveRoom() {
    if (!m_inRoom)
        return;

    if (m_networkManager != nullptr && m_networkManager->isConnected()) {
        QJsonObject message;
        message["type"] = "leave_room";
        send(message);
    }

    clearRoomState();
    emit roomLeft();
}

void RoomManager::setRoomMode(RoomMode mode) {
    if (!m_inRoom) {
        emit errorOccurred("你不在房间里");
        return;
    }
    if (!isHost()) {
        emit errorOccurred("只有房主可以更改房间模式");
        return;
    }

    QJsonObject message;
    message["type"] = "set_room_mode";

    if (mode == RoomMode::Local) {
        message["mode"] = "local";
    } else if (mode == RoomMode::Share) {
        message["mode"] = "share";
    } else if (mode == RoomMode::Url) {
        message["mode"] = "url";
    } else {
        emit errorOccurred("未知的房间模式");
        return;
    }
    send(message);
    // 等服务器的 room_mode_changed 广播修改
}


// ============================
// 房间内消息
// ============================

void RoomManager::sendChat(const QString &text) {
    if (text.trimmed().isEmpty())
        return;

    if (!m_inRoom) {
        emit errorOccurred("你不在房间里");
        return;
    }

    QJsonObject message;
    message["type"] = "chat";
    message["text"] = text.trimmed();
    send(message);
}

void RoomManager::sendPlayback(const QString &action, qint64 position) {
    if (!m_inRoom)
        return;

    QJsonObject message;
    message["type"] = "playback";
    message["action"] = action;
    message["position"] = position;
    send(message);
}

void RoomManager::sendWebrtcOffer(const QString &to, const QString &sdp) {
    if (!m_inRoom)
        return;

    QJsonObject message;
    message["type"] = "webrtc_offer";
    message["to"] = to;
    message["sdp"] = sdp;
    send(message);
}

void RoomManager::sendWebrtcAnswer(const QString &to, const QString &sdp) {
    if (!m_inRoom)
        return;

    QJsonObject message;
    message["type"] = "webrtc_answer";
    message["to"] = to;
    message["sdp"] = sdp;
    send(message);
}

void RoomManager::sendWebrtcIce(const QString &to, const QString &sdp,
                                const QString &sdpMid, int sdpMLineIndex) {
    if (!m_inRoom)
        return;

    QJsonObject message;
    message["type"] = "webrtc_ice";
    message["to"] = to;
    message["sdp"] = sdp;
    message["sdpMid"] = sdpMid;
    message["sdpMLineIndex"] = sdpMLineIndex;
    send(message);
}


// ============================
// 发消息
// ============================

void RoomManager::send(const QJsonObject &message) {
    if (m_networkManager == nullptr || !m_networkManager->isConnected()) {
        emit errorOccurred("未连接到服务器");
        return;
    }

    m_networkManager->sendMessage(
        QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void RoomManager::sendPendingRequest() {
    if (m_pendingAction == PendingAction::None)
        return;

    if (m_networkManager == nullptr) {
        failPending("网络模块未初始化");
        return;
    }

    // 还没连上服务器:先建立连接,连接成功后 onNetworkConnected 会再次调用这里
    if (!m_networkManager->isConnected()) {
        m_networkManager->connectToServer();
        return;
    }

    QJsonObject message;
    if (m_pendingAction == PendingAction::CreateRoom) {
        message["type"] = "create_room";
        message["roomId"] = m_pendingRoomId;
        message["roomName"] = m_pendingRoomName;
    } else {
        message["type"] = "join_room";
        message["roomId"] = m_pendingRoomId;
    }

    message["nickname"] = m_pendingNickname;
    if (!m_pendingPassword.isEmpty())
        message["password"] = m_pendingPassword;

    send(message);

    m_pendingAction = PendingAction::None;
    setAwaitingReply(true);
}

void RoomManager::failPending(const QString &error) {
    m_pendingAction = PendingAction::None;
    emit isConnectingChanged();
    emit errorOccurred(error);
}

void RoomManager::setAwaitingReply(bool awaiting) {
    if (m_awaitingReply == awaiting)
        return;

    m_awaitingReply = awaiting;
    emit isConnectingChanged();
}


// ============================
// 连接状态变化
// ============================

void RoomManager::onNetworkConnected() {
    sendPendingRequest();
}

void RoomManager::onNetworkDisconnected() {
    if (m_pendingAction != PendingAction::None)
        failPending("无法连接到服务器");

    if (m_awaitingReply)
        setAwaitingReply(false);

    if (m_inRoom) {
        clearRoomState();
        emit roomLeft();
        emit errorOccurred("与服务器的连接已断开");
    }
}

// ============================
// 播放器源变化
// ============================

void RoomManager::onPlaybackSourceChanged() {
    if (!m_inRoom || m_playbackController == nullptr)
        return;

    const bool loaded = m_playbackController->hasLoaded();

    QJsonObject message;
    message["type"] = "video_status";
    message["loaded"] = loaded;
    message["hash"] = loaded ? m_playbackController->hash() : QString();
    message["duration"] = loaded ? m_playbackController->duration() : 0;
    send(message);

    emit allLoadedChanged();
}


// ============================
// 解析服务端消息
// ============================

void RoomManager::handleMessage(const QString &text) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        emit errorOccurred("收到无法解析的消息");
        return;
    }

    const QJsonObject message = doc.object();
    const QString type = message.value("type").toString();

    if (type == "welcome") {
        m_clientId = message.value("clientId").toString();
        emit clientIdChanged();
        return;
    }

    if (type == "room_created" || type == "room_joined") {
        m_roomId = message.value("roomId").toString();
        m_roomName = message.value("roomName").toString();
        applyMembers(message.value("members").toArray());

        m_roomMode = modeFromString(message.value("mode").toString());
        const bool mismatched = message.value("videoMismatched").toBool(false);
        const qint64 shortest =
            static_cast<qint64>(message.value("shortestDuration").toDouble());
        m_videoMismatched = mismatched;
        m_shortestDuration = shortest;

        m_inRoom = true;
        setAwaitingReply(false);
        emit roomIdChanged();
        emit roomNameChanged();
        emit roomModeChanged();
        emit videoMismatchChanged();

        if (type == "room_created")
            emit roomCreated();
        else
            emit roomJoined();

        // 向房间报告自己的视频加载状态(可能进房前就已加载)
        // onPlaybackSourceChanged();

        // 服务端附带房间当前播放状态,新成员据此对齐
        // 用 singleShot 延迟发出:页面在 roomJoined 信号里才被 push,
        // 等 RoomPage 建好 Connections 之后再投递
        const QJsonObject state = message.value("state").toObject();
        if (!state.isEmpty()) {
            const qint64 position =
                static_cast<qint64>(state.value("position").toDouble());
            const bool playing = state.value("playing").toBool();
            QTimer::singleShot(0, this, [this, position, playing]() {
                emit playbackReceived("seek", position);
                emit playbackReceived(playing ? "play" : "pause", position);
            });
        }
        return;
    }

    if (type == "room_left") {
        clearRoomState();
        emit roomLeft();
        return;
    }

    if (type == "member_joined") {
        const QString id = message.value("clientId").toString();
        const QString nick = message.value("nickname").toString();

        // 自己加入时 room_joined 已带完整成员列表,这里会重复,跳过
        for (const QVariant &value : m_members) {
            if (value.toMap().value("clientId").toString() == id)
                return;
        }

        QVariantMap member;
        member["clientId"] = id;
        member["nickname"] = nick;
        member["isHost"] = message.value("isHost").toBool(false);
        member["loaded"] = message.value("loaded").toBool(false);
        member["duration"] =
            static_cast<qint64>(message.value("duration").toDouble());
        m_members.append(member);

        emit membersChanged();
        emit isHostChanged();
        emit memberJoined(nick);
        emit otherLoadedChanged();
        emit allLoadedChanged();
        return;
    }

    if (type == "member_left") {
        const QString id = message.value("clientId").toString();
        for (int i = 0; i < m_members.size(); ++i) {
            if (m_members.at(i).toMap().value("clientId").toString() == id) {
                const QString nick =
                    m_members.at(i).toMap().value("nickname").toString();
                m_members.removeAt(i);

                emit membersChanged();
                emit isHostChanged();
                emit memberLeft(nick);
                emit otherLoadedChanged();
                emit allLoadedChanged();
                break;
            }
        }
        return;
    }

    if (type == "chat") {
        emit chatReceived(message.value("from").toString(),
                          message.value("text").toString());
        return;
    }

    if (type == "playback") {
        const qint64 position =
            static_cast<qint64>(message.value("position").toDouble());
        emit playbackReceived(message.value("action").toString(), position);
        return;
    }

    if (type == "room_mode_changed") {
        const RoomMode newMode = modeFromString(message.value("mode").toString());
        if (newMode != m_roomMode) {
            m_roomMode = newMode;
            emit roomModeChanged();
        }
        return;
    }

    if (type == "video_status") {
        const QString id = message.value("clientId").toString();
        const bool loaded = message.value("loaded").toBool();
        const qint64 duration =
            static_cast<qint64>(message.value("duration").toDouble());

        for (int i = 0; i < m_members.size(); ++i) {
            QVariantMap member = m_members.at(i).toMap();
            if (member.value("clientId").toString() == id) {
                member["loaded"] = loaded;
                member["duration"] = duration;
                m_members[i] = member;
                break;
            }
        }

        emit membersChanged();
        emit otherLoadedChanged();
        emit allLoadedChanged();
        return;
    }

    if (type == "video_mismatch") {
        const bool mismatched = message.value("mismatched").toBool();
        const qint64 shortest =
            static_cast<qint64>(message.value("shortestDuration").toDouble());
        if (mismatched != m_videoMismatched || shortest != m_shortestDuration) {
            m_videoMismatched = mismatched;
            m_shortestDuration = shortest;
            emit videoMismatchChanged();
        }
        return;
    }

    if (type == "webrtc_offer" || type == "webrtc_answer" || type == "webrtc_ice") {
        const QString from = message.value("from").toString();
        const QString sdp = message.value("sdp").toString();
        if (type == "webrtc_offer") {
            emit webrtcOfferReceived(from, sdp);
        } else if (type == "webrtc_answer") {
            emit webrtcAnswerReceived(from, sdp);
        } else {
            emit webrtcIceReceived(from, sdp,
                                   message.value("sdpMid").toString(),
                                   message.value("sdpMLineIndex").toInt());
        }
        return;
    }

    if (type == "error") {
        if (m_awaitingReply)
            setAwaitingReply(false);
        emit errorOccurred(message.value("message").toString());
    }
}


// ============================
// 内部辅助
// ============================

void RoomManager::applyMembers(const QJsonArray &membersArray) {
    m_members.clear();
    for (const QJsonValue &value : membersArray) {
        const QJsonObject memberObject = value.toObject();
        QVariantMap member;
        member["clientId"] = memberObject.value("clientId").toString();
        member["nickname"] = memberObject.value("nickname").toString();
        member["isHost"] = memberObject.value("isHost").toBool(false);
        member["loaded"] = memberObject.value("loaded").toBool(false);
        member["duration"] =
            static_cast<qint64>(memberObject.value("duration").toDouble());
        m_members.append(member);
    }
    emit membersChanged();
    emit isHostChanged();
    emit otherLoadedChanged();
    emit allLoadedChanged();
}

void RoomManager::clearRoomState() {
    m_roomId.clear();
    m_roomName.clear();
    m_members.clear();
    m_inRoom = false;

    const bool modeChanged = m_roomMode != RoomMode::Local;
    m_roomMode = RoomMode::Local;
    const bool mismatchChanged =
        m_videoMismatched || m_shortestDuration != 0;
    m_videoMismatched = false;
    m_shortestDuration = 0;

    emit roomIdChanged();
    emit roomNameChanged();
    emit inRoomChanged();
    emit membersChanged();
    emit isHostChanged();
    if (modeChanged)
        emit roomModeChanged();
    if (mismatchChanged)
        emit videoMismatchChanged();
    emit otherLoadedChanged();
    emit allLoadedChanged();
}
