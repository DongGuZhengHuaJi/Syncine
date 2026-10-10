//
// Created by donggu on 2026/9/13.
//

#include "RoomSession.h"

#include <QFileInfo>
#include <QUuid>

#include "core/Log.h"
#include "core/MediaHash.h"
#include "core/NetworkManager.h"

RoomSession::RoomSession(NetworkManager *networkManager, QObject *parent)
    : QObject(parent),
      m_networkManager(networkManager),
      m_members(new MemberModel(this)),
      m_playlist(new PlaylistModel(this)) {

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

PlaylistModel *RoomSession::playlist() const {
    return m_playlist;
}

QString RoomSession::localFileFor(const QString &itemId) const {
    return m_playlist->localFile(itemId);
}

QString RoomSession::urlFor(const QString &itemId) const {
    const QList<PlaylistEntry> &entries = m_playlist->entries();
    for (const PlaylistEntry &entry : entries) {
        if (entry.itemId == itemId)
            return entry.url;
    }
    return {};
}

bool RoomSession::videoMismatched() const {
    return m_videoMismatched;
}

qint64 RoomSession::shortestDuration() const {
    return m_shortestDuration;
}

QList<QString> RoomSession::otherMemberIds() const {
    return m_members->idsExcept(m_clientId);
}

bool RoomSession::allOthersLoaded() const {
    return m_members->allOthersLoaded(m_clientId);
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

    // 播放列表随房间状态一起下发:队列 + 现在放到第几条
    m_playlist->reset(snapshot.playlist, snapshot.currentIndex);

    // 先发 entered,让 PlaybackSync 有机会上报自己的视频状态
    emit entered();

    // 服务端附带房间当前播放状态,新成员据此对齐:先 seek 再 play/pause
    if (snapshot.hasState) {
        emit playbackCommandReceived(PlaybackAction::Seek, snapshot.position);
        emit playbackCommandReceived(
            snapshot.playing ? PlaybackAction::Play : PlaybackAction::Pause,
            snapshot.position);
    }

    // 入房时房间可能已经在放某一条了 —— 通知下游去解析并加载
    // (本地模式下就是"请选择你这台机器上的对应文件")
    const QString currentItem = m_playlist->currentItemId();
    if (!currentItem.isEmpty())
        emit playlistSwitched(currentItem);
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
    m_playlist->clear();    // 播放列表和本地文件映射都是跟着房间走的

    const bool modeChanged = m_roomMode != RoomMode::Sync;
    m_roomMode = RoomMode::Sync;

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

void RoomSession::sendPlaybackPosition(qint64 position, bool playing) {
    if (!m_inRoom)
        return;

    send(Protocol::encodePlaybackPosition(position, playing));
}

void RoomSession::reportVideoStatus(bool loaded, const QString &hash, qint64 duration) {
    if (!m_inRoom)
        return;

    // 先更新本地成员表
    m_members->setVideoStatus(m_clientId, loaded, duration);

    send(Protocol::encodeVideoStatus(loaded, hash, duration));
}

// ============================
// 播放列表
// ============================

void RoomSession::addLocalFile(const QUrl &fileUrl) {
    if (!m_inRoom || !fileUrl.isValid())
        return;

    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty())
        return;

    const QString itemId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // **先记本地映射再发网络**:服务端会立刻把权威列表广播回来(包括发给我自己),
    // 那一刻本机必须已经能把"这条 → 我的文件"解析出来,否则会闪一下"未匹配"
    m_playlist->setLocalFile(itemId, path);

    send(Protocol::encodePlaylistAdd(itemId, QFileInfo(path).fileName(),
                                     QString(), 0));
    reportPlaylistFile(itemId, path);
}

void RoomSession::reportPlaylistFile(const QString &itemId, const QString &path) {
    if (!m_inRoom || itemId.isEmpty())
        return;

    // 只有同步模式需要"匹配":共享模式里条目就是房主自己的文件,
    // 网链模式大家读的是同一个 URL,都不存在"你有没有"这个问题
    if (m_roomMode != RoomMode::Sync)
        return;

    const QString hash = MediaHash::forFile(path);
    if (hash.isEmpty()) {
        // 文件读不了(被删/没权限)—— 如实报"没有",让状态显示成未匹配,
        // 而不是报个空哈希冒充"有"
        LOG_WARN("Playlist") << "指定的文件读不了,按未匹配上报:" << path;
    }

    send(Protocol::encodePlaylistStatus(itemId, !hash.isEmpty(), hash));
}

void RoomSession::addUrl(const QString &url, const QString &title) {
    if (!m_inRoom || url.isEmpty())
        return;

    const QString itemId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    send(Protocol::encodePlaylistAdd(itemId,
                                     title.isEmpty() ? url : title,
                                     url, 0));
}

void RoomSession::assignLocalFile(const QString &itemId, const QUrl &fileUrl) {
    if (itemId.isEmpty() || !fileUrl.isValid())
        return;

    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty())
        return;

    m_playlist->setLocalFile(itemId, path);
    // 每次指定/重选都重新上报(哈希可能变了 —— 用户可能换了个文件)
    reportPlaylistFile(itemId, path);

    // 补的正好是当前条目(比如入房后才发现要选文件)→ 立刻重新解析并加载。
    // 复用 playlistSwitched 这条路径,下游不用再加一个分支。
    if (itemId == m_playlist->currentItemId())
        emit playlistSwitched(itemId);
}

void RoomSession::removeFromPlaylist(const QString &itemId) {
    if (!m_inRoom || itemId.isEmpty())
        return;

    send(Protocol::encodePlaylistRemove(itemId));
}

void RoomSession::switchTo(const QString &itemId) {
    if (!m_inRoom || itemId.isEmpty())
        return;

    send(Protocol::encodePlaylistSwitch(itemId));
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

    case Protocol::MessageType::PlaybackPosition:
        emit playbackPositionReceived(message->position, message->playing);
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

    case Protocol::MessageType::PlaylistChanged:
        handlePlaylistChanged(*message);
        break;

    case Protocol::MessageType::PlaylistSwitched:
        handlePlaylistSwitched(*message);
        break;

    case Protocol::MessageType::PlaylistStatus:
        handlePlaylistStatus(*message);
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

    // 条目时长在添加时是不知道的 —— 谁把它加载起来,谁就顺手把时长告诉列表。
    // video_status 不带 itemId,但只有"当前条目"会被加载,所以对应到当前条目。
    if (message.loaded && message.duration > 0) {
        const QString itemId = m_playlist->currentItemId();
        if (!itemId.isEmpty())
            m_playlist->setDuration(itemId, message.duration);
    }
}

void RoomSession::handlePlaylistChanged(const Protocol::Message &message) {
    m_playlist->setEntries(message.playlist, message.currentIndex);
}

void RoomSession::handlePlaylistSwitched(const Protocol::Message &message) {
    m_playlist->setCurrentIndex(message.currentIndex);

    // 以 model 里的状态为准(itemId 和 currentIndex 一致);
    // 列表空了就是空串 —— 下游据此把播放器卸载掉
    emit playlistSwitched(m_playlist->currentItemId());
}

void RoomSession::handlePlaylistStatus(const Protocol::Message &message) {
    // 服务端聚合好的"每条匹配到哪一步",直接铺到 model 上。
    // 打日志是为了排查"为什么这条显示未匹配" —— 光看界面看不出是谁还没选文件
    for (const PlaylistEntry &entry : message.playlist)
        LOG_DEBUG("Playlist") << "匹配状态:" << entry.itemId.left(8) << "→" << entry.status;

    m_playlist->applyStatuses(message.playlist);
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
    case RoomMode::Sync:
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
    return RoomMode::Sync;
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
