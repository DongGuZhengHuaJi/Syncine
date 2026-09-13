//
// Created by donggu on 2026/9/13.
//

#include "RoomSession.h"

#include "core/NetworkManager.h"

RoomSession::RoomSession(NetworkManager *networkManager, QObject *parent)
    : QObject(parent),
      m_networkManager(networkManager),
      m_members(new MemberModel(this)) {

    if (m_networkManager != nullptr) {
        connect(m_networkManager, &NetworkManager::messageReceived,
                this, &RoomSession::onMessage);
    }

    // 成员表一变就重算派生量,调用方不需要记得补 emit
    connect(m_members, &MemberModel::changed,
            this, &RoomSession::refreshDerivedState);
}

// ============================
// 属性
// ============================

QString RoomSession::roomId() const {
    return m_roomId;
}

QString RoomSession::roomName() const {
    return m_roomName;
}

bool RoomSession::inRoom() const {
    return m_inRoom;
}

QString RoomSession::clientId() const {
    return m_clientId;
}

RoomSession::RoomMode RoomSession::roomMode() const {
    return m_roomMode;
}

bool RoomSession::isHost() const {
    return m_members->isHost(m_clientId);
}

MemberModel *RoomSession::members() const {
    return m_members;
}

bool RoomSession::videoMismatched() const {
    return m_videoMismatched;
}

qint64 RoomSession::shortestDuration() const {
    return m_shortestDuration;
}

bool RoomSession::allOthersLoaded() const {
    return m_members->allOthersLoaded(m_clientId);
}

QString RoomSession::firstOtherMemberId() const {
    return m_members->firstIdExcept(m_clientId);
}

// ============================
// 生命周期
// ============================

void RoomSession::enterRoom(const RoomSnapshot &snapshot) {
    m_roomId = snapshot.roomId;
    m_roomName = snapshot.roomName;
    m_clientId = snapshot.clientId;
    m_members->reset(snapshot.members);

    const RoomMode mode = modeFromWire(snapshot.mode);
    const bool modeChanged = m_roomMode != mode;
    m_roomMode = mode;

    const bool mismatchChanged = m_videoMismatched != snapshot.videoMismatched
                                 || m_shortestDuration != snapshot.shortestDuration;
    m_videoMismatched = snapshot.videoMismatched;
    m_shortestDuration = snapshot.shortestDuration;

    const bool wasInRoom = m_inRoom;
    m_inRoom = true;

    emit roomIdChanged();
    emit roomNameChanged();
    emit clientIdChanged();
    if (modeChanged)
        emit roomModeChanged();
    if (mismatchChanged)
        emit videoMismatchChanged();
    if (!wasInRoom)
        emit inRoomChanged();

    // 先发 entered,让 PlaybackSync 有机会上报自己的视频状态
    emit entered();

    // 服务端附带房间当前播放状态,新成员据此对齐:先 seek 再 play/pause
    if (snapshot.hasState) {
        emit playbackCommandReceived(PlaybackAction::Seek, snapshot.position);
        emit playbackCommandReceived(
            snapshot.playing ? PlaybackAction::Play : PlaybackAction::Pause,
            snapshot.position);
    }
}

void RoomSession::leaveRoom() {
    if (!m_inRoom)
        return;

    clearRoomState();
    emit left();
}

void RoomSession::clearRoomState() {
    m_roomId.clear();
    m_roomName.clear();
    m_inRoom = false;
    m_members->clear();     // 触发 refreshDerivedState,顺带修正 isHost

    const bool modeChanged = m_roomMode != RoomMode::Local;
    m_roomMode = RoomMode::Local;

    const bool mismatchChanged = m_videoMismatched || m_shortestDuration != 0;
    m_videoMismatched = false;
    m_shortestDuration = 0;

    emit roomIdChanged();
    emit roomNameChanged();
    emit inRoomChanged();
    if (modeChanged)
        emit roomModeChanged();
    if (mismatchChanged)
        emit videoMismatchChanged();
}

// ============================
// 房间内操作
// ============================

void RoomSession::setRoomMode(RoomMode mode) {
    if (!m_inRoom) {
        emit errorOccurred("你不在房间里");
        return;
    }
    if (!isHost()) {
        emit errorOccurred("只有房主可以更改房间模式");
        return;
    }

    // 本地状态等服务端的 room_mode_changed 广播回来再改,保证全员一致
    if (!send(Protocol::encodeSetRoomMode(modeToWire(mode))))
        emit errorOccurred("未连接到服务器");
}

void RoomSession::sendChat(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;

    if (!m_inRoom) {
        emit errorOccurred("你不在房间里");
        return;
    }

    if (!send(Protocol::encodeChat(trimmed)))
        emit errorOccurred("未连接到服务器");
}

void RoomSession::sendPlayback(PlaybackAction action, qint64 position) {
    if (!m_inRoom)
        return;

    send(Protocol::encodePlayback(actionToWire(action), position));
}

void RoomSession::reportVideoStatus(bool loaded, const QString &hash, qint64 duration) {
    if (!m_inRoom)
        return;

    send(Protocol::encodeVideoStatus(loaded, hash, duration));
}

// ============================
// 消息分发
// ============================

void RoomSession::onMessage(const QString &text) {
    const std::optional<Protocol::Message> message = Protocol::decode(text);
    if (!message) {
        // 解析失败由 SessionController 报告(它是消息管道的唯一入口),
        // 这里静默跳过,避免同一个错误弹两次
        return;
    }

    switch (message->type) {
    case Protocol::MessageType::MemberJoined:
        handleMemberJoined(*message);
        break;

    case Protocol::MessageType::MemberLeft:
        handleMemberLeft(*message);
        break;

    case Protocol::MessageType::Chat:
        emit chatReceived(message->from, message->text);
        break;

    case Protocol::MessageType::Playback:
        handlePlayback(*message);
        break;

    case Protocol::MessageType::RoomModeChanged: {
        const RoomMode mode = modeFromWire(message->mode);
        if (mode != m_roomMode) {
            m_roomMode = mode;
            emit roomModeChanged();
        }
        break;
    }

    case Protocol::MessageType::VideoStatus:
        handleVideoStatus(*message);
        break;

    case Protocol::MessageType::VideoMismatch:
        handleVideoMismatch(*message);
        break;

    default:
        // welcome / room_created / room_joined / room_left / error
        // 属于会话建立阶段,归 SessionController
        break;
    }
}

void RoomSession::handleMemberJoined(const Protocol::Message &message) {
    Member member;
    member.clientId = message.clientId;
    member.nickname = message.nickname;
    member.isHost = message.isHost;
    member.loaded = message.loaded;
    member.duration = message.duration;

    // 自己加入时 room_joined 已经带了完整名单,这里会重复;
    // MemberModel::append 对已存在的 clientId 直接忽略
    if (!m_members->append(member))
        return;

    emit memberJoined(member.nickname);
}

void RoomSession::handleMemberLeft(const Protocol::Message &message) {
    const std::optional<Member> removed = m_members->remove(message.clientId);
    if (!removed)
        return;

    emit memberLeft(removed->nickname);
}

void RoomSession::handlePlayback(const Protocol::Message &message) {
    emit playbackCommandReceived(actionFromWire(message.action), message.position);
}

void RoomSession::handleVideoStatus(const Protocol::Message &message) {
    m_members->setVideoStatus(message.clientId, message.loaded, message.duration);
}

void RoomSession::handleVideoMismatch(const Protocol::Message &message) {
    if (message.videoMismatched == m_videoMismatched
        && message.shortestDuration == m_shortestDuration) {
        return;
    }

    m_videoMismatched = message.videoMismatched;
    m_shortestDuration = message.shortestDuration;
    emit videoMismatchChanged();
}

// ============================
// 内部辅助
// ============================

void RoomSession::refreshDerivedState() {
    const bool host = isHost();
    if (host != m_isHost) {
        m_isHost = host;
        emit isHostChanged();
    }

    emit membersChanged();
}

bool RoomSession::send(const QString &text) {
    if (m_networkManager == nullptr)
        return false;

    return m_networkManager->sendMessage(text);
}

QString RoomSession::modeToWire(RoomMode mode) {
    switch (mode) {
    case RoomMode::Local:
        return QStringLiteral("local");
    case RoomMode::Share:
        return QStringLiteral("share");
    case RoomMode::Url:
        return QStringLiteral("url");
    }
    return QStringLiteral("local");
}

RoomSession::RoomMode RoomSession::modeFromWire(const QString &mode) {
    if (mode == QLatin1String("share"))
        return RoomMode::Share;
    if (mode == QLatin1String("url"))
        return RoomMode::Url;
    return RoomMode::Local;
}

QString RoomSession::actionToWire(PlaybackAction action) {
    switch (action) {
    case PlaybackAction::Play:
        return QStringLiteral("play");
    case PlaybackAction::Pause:
        return QStringLiteral("pause");
    case PlaybackAction::Seek:
        return QStringLiteral("seek");
    }
    return QStringLiteral("seek");
}

RoomSession::PlaybackAction RoomSession::actionFromWire(const QString &action) {
    if (action == QLatin1String("play"))
        return PlaybackAction::Play;
    if (action == QLatin1String("pause"))
        return PlaybackAction::Pause;
    return PlaybackAction::Seek;
}
