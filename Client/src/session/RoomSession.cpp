//
// Created by donggu on 2026/9/13.
//

#include "RoomSession.h"

#include <QCryptographicHash>
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

    // 成员表一变就重算派生量，调用方不需要记得补 emit
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

bool RoomSession::isMemberOnline(const QString &clientId) const {
    return m_members != nullptr && m_members->isOnline(clientId);
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

QList<IceServerConfig> RoomSession::iceServers() const {
    return m_iceServers;
}

bool RoomSession::allOthersLoaded() const {
    return m_members->allOthersLoaded(m_clientId);
}

// ============================
// 生命周期
// ============================

void RoomSession::enterRoom(const RoomSnapshot &snapshot) {
    // 断线重连会再次走到这里。若房间、模式和当前条目都没变，说明本地播放器
    // 一直在跑，不能再套用服务端快照里的位置 —— Sync 模式没有周期位置广播，
    const bool sameRoom = m_inRoom && m_roomId == snapshot.roomId;
    const QString previousItem =
        m_playlist != nullptr ? m_playlist->currentItemId() : QString();
    const RoomMode previousMode = m_roomMode;

    // resume 后 clientId 不变，不能发 clientIdChanged：SignalingChannel 会因此
    // 拆掉所有 PeerConnection。只有普通 join / 换房才需要重建。
    const bool clientIdDidChange = (m_clientId != snapshot.clientId);

    m_roomId = snapshot.roomId;
    m_roomName = snapshot.roomName;
    m_clientId = snapshot.clientId;

    // ICE 配置必须先于成员表赋值：m_members->reset() 会发出 membersChanged，
    // SignalingChannel 收到后调 syncPeers() 建连。首次进房那次会被
    // onMembersChanged 中「不在房间则返回」挡掉，但换房时（已在房间 A 直接进
    // 房间 B）m_inRoom 仍为 true，该次建连会真实发生——若配置赋值在后，
    // 就会带着上一个房间的凭据建连，且表面上无任何异常。
    m_iceServers = snapshot.iceServers;

    // ICE 配置在此落账并记录一行：跨网络连接失败时，「凭据是否下发」是需要
    // 首先确认的事项。记录时机选在接收当下而非建连时——单独一人在房间内不会
    // 建立 PeerConnection，该情况下也需要可见。
    //
    // 只记录 URL：username/credential 是临时密钥，任何级别都不写入日志。
    if (m_iceServers.isEmpty()) {
        LOG_WARN("Room") << "服务端未下发 ICE 配置，只能直连（对称 NAT 后的对端将无法连接）";
    } else {
        QStringList groups;
        for (const IceServerConfig &server : m_iceServers) {
            groups.append(server.urls.join(QLatin1Char(' '))
                          + (server.credential.isEmpty()
                                 ? QStringLiteral(" [STUN]")
                                 : QStringLiteral(" [TURN 带凭据]")));
        }
        LOG_INFO("Room") << "收到 ICE 配置:" << groups.join(QStringLiteral(" | "));
    }

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
    if (clientIdDidChange)
        emit clientIdChanged();
    if (modeChanged)
        emit roomModeChanged();
    if (mismatchChanged)
        emit videoMismatchChanged();
    if (!wasInRoom)
        emit inRoomChanged();

    // 播放列表随房间状态一并下发：队列 + 当前条目
    m_playlist->reset(snapshot.playlist, snapshot.currentIndex);

    // 先发 entered，使 PlaybackSync 有机会上报自身的视频状态
    emit entered();

    // 服务端附带房间当前播放状态，新成员据此对齐：先换挡，再 seek，最后 play/pause。
    // 同一房间的断线重连（模式、当前条目都没变）跳过：本地播放器还在跑，
    // 快照里的位置可能是旧的。URL/共享模式随后会有房主的位置广播来纠偏。
    const QString currentItem = m_playlist->currentItemId();
    const bool samePlaybackContext =
        sameRoom && previousItem == currentItem && previousMode == m_roomMode;

    if (snapshot.hasState && !samePlaybackContext) {
        emit playbackCommandReceived(PlaybackAction::Rate,
                                     snapshot.position, snapshot.rate);
        emit playbackCommandReceived(PlaybackAction::Seek, snapshot.position,
                                     snapshot.rate);
        emit playbackCommandReceived(
            snapshot.playing ? PlaybackAction::Play : PlaybackAction::Pause,
            snapshot.position, snapshot.rate);
    } else if (samePlaybackContext) {
        LOG_DEBUG("Room") << "断线重连且当前条目未变,保留本地播放状态";
    }

    // 入房时房间可能已在播放某一条，通知下游解析并加载
    // （本地模式下即为「请选择本机对应的文件」）。
    // 重连时条目没变就不再触发 load，避免把正在播放的媒体重新加载一遍。
    if (!currentItem.isEmpty() && !samePlaybackContext)
        emit playlistSwitched(currentItem);

    LOG_INFO("Room") << "进入房间完成: roomId=" << m_roomId
                     << " mode=" << snapshot.mode
                     << " 成员=" << snapshot.members.size()
                     << " 当前条目=" << (currentItem.isEmpty() ? "(无)" : currentItem);
}

void RoomSession::leaveRoom() {
    if (!m_inRoom)
        return;

    LOG_INFO("Room") << "退出房间: roomId=" << m_roomId;
    clearRoomState();
    emit left();
}

void RoomSession::clearRoomState() {
    m_roomId.clear();
    m_roomName.clear();
    m_inRoom = false;
    m_iceServers.clear();   // 凭据随本次入房而来，退房即作废（下次进房会重新下发）
    m_members->clear();     // 触发 refreshDerivedState，同时修正 isHost
    m_playlist->clear();    // 播放列表与本地文件映射都随房间走

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
        LOG_WARN("Room") << "切换模式被拒：当前不在房间中";
        emit errorOccurred("当前不在房间中");
        return;
    }
    if (!isHost()) {
        LOG_WARN("Room") << "切换模式被拒：非房主";
        emit errorOccurred("只有房主可以更改房间模式");
        return;
    }

    LOG_INFO("Room") << "请求切换房间模式: " << modeToWire(mode);
    // 本地状态等服务端的 room_mode_changed 广播回来再改，保证全员一致
    if (!send(Protocol::encodeSetRoomMode(modeToWire(mode))))
        emit errorOccurred("未连接到服务器");
}

void RoomSession::sendChat(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return;

    if (!m_inRoom) {
        LOG_WARN("Room") << "发送聊天被拒：当前不在房间中";
        emit errorOccurred("当前不在房间中");
        return;
    }

    if (!send(Protocol::encodeChat(trimmed)))
        emit errorOccurred("未连接到服务器");
}

void RoomSession::sendPlayback(PlaybackAction action, qint64 position, qreal rate) {
    if (!m_inRoom)
        return;

    send(Protocol::encodePlayback(actionToWire(action), position, rate));
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

    // 必须先记本地映射再发网络：服务端会立即把权威列表广播回来（含发给自身），
    // 那一刻本机必须已能将「该条 → 我的文件」解析出来，否则会短暂显示为未匹配
    m_playlist->setLocalFile(itemId, path);

    LOG_INFO("Room") << "添加本地文件到播放列表: itemId=" << itemId
                     << " 文件=" << QFileInfo(path).fileName();
    send(Protocol::encodePlaylistAdd(itemId, QFileInfo(path).fileName(),
                                     QString(), 0, /*live=*/false, /*spec=*/QString()));
    reportPlaylistFile(itemId, path);
}

void RoomSession::reportPlaylistFile(const QString &itemId, const QString &path) {
    if (!m_inRoom || itemId.isEmpty())
        return;

    // 仅同步模式需要「匹配」：共享模式下条目就是房主自己的文件，
    // 网链模式各端读同一 URL，都不存在「是否持有」的问题
    if (m_roomMode != RoomMode::Sync)
        return;

    const QString hash = MediaHash::forFile(path);
    if (hash.isEmpty()) {
        // 文件不可读（被删/无权限）时如实上报「没有」，使状态显示为未匹配，
        // 而不是上报空哈希冒充「有」
        LOG_WARN("Playlist") << "文件不可读，按未匹配上报: itemId=" << itemId
                             << " 路径=" << path;
    }

    send(Protocol::encodePlaylistStatus(itemId, !hash.isEmpty(), hash));
}

void RoomSession::addUrlWithId(const QString &itemId, const QString &url,
                               const QString &title, bool live,
                               qint64 durationMs, const QString &spec) {
    if (!m_inRoom || url.isEmpty() || itemId.isEmpty())
        return;

    LOG_INFO("Room") << "添加网链条目: itemId=" << itemId
                     << " live=" << (live ? 1 : 0)
                     << " spec=" << (spec.isEmpty() ? "(默认)" : spec)
                     << " title=" << title;
    send(Protocol::encodePlaylistAdd(itemId,
                                     title.isEmpty() ? url : title,
                                     url, durationMs, live, spec));
}

void RoomSession::addUrl(const QString &url, const QString &title, bool live,
                         qint64 durationMs, const QString &spec) {
    if (!m_inRoom || url.isEmpty())
        return;

    // itemId 由 URL 计算：同一链接即同一条目，各端各加一次不会变成两条
    // （服务端按 itemId 查重）。使用哈希而非原样携带 url，因为该值要进入协议。
    // 剧集不走此路径，它们使用平台集号（见 FormatModel 的 Episodes::itemId）。
    const QByteArray digest = QCryptographicHash::hash(url.toUtf8(),
                                                       QCryptographicHash::Sha1);
    const QString itemId = QStringLiteral("url")
                           + QString::fromLatin1(digest.toHex().left(16));
    addUrlWithId(itemId, url, title, live, durationMs, spec);
}

void RoomSession::assignLocalFile(const QString &itemId, const QUrl &fileUrl) {
    if (itemId.isEmpty() || !fileUrl.isValid())
        return;

    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty())
        return;

    LOG_INFO("Room") << "为条目指定本地文件: itemId=" << itemId << " 文件=" << path;
    m_playlist->setLocalFile(itemId, path);
    // 每次指定或重选都重新上报（用户可能更换了文件，哈希随之改变）
    reportPlaylistFile(itemId, path);

    // 若补上的正是当前条目（例如入房后才发现需要选文件），立即重新解析并加载。
    // 复用 playlistSwitched 这条路径，下游无需新增分支。
    if (itemId == m_playlist->currentItemId())
        emit playlistSwitched(itemId);
}

void RoomSession::removeFromPlaylist(const QString &itemId) {
    if (!m_inRoom || itemId.isEmpty())
        return;

    LOG_INFO("Room") << "从播放列表移除条目: itemId=" << itemId;
    send(Protocol::encodePlaylistRemove(itemId));
}

void RoomSession::switchTo(const QString &itemId) {
    if (!m_inRoom || itemId.isEmpty())
        return;

    LOG_INFO("Room") << "请求切换到条目: itemId=" << itemId;
    send(Protocol::encodePlaylistSwitch(itemId));
}

// ============================
// 消息分发
// ============================

void RoomSession::onMessage(const QString &text) {
    const std::optional<Protocol::Message> message = Protocol::decode(text);
    if (!message) {
        // 解析失败由 SessionController 上报（它是消息管道的唯一入口），
        // 此处静默跳过，避免同一错误提示两次
        return;
    }

    switch (message->type) {
    case Protocol::MessageType::MemberJoined:
        handleMemberJoined(*message);
        break;

    case Protocol::MessageType::MemberLeft:
        handleMemberLeft(*message);
        break;

    case Protocol::MessageType::MemberOnline:
        handleMemberOnline(*message);
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
        // 属会话建立阶段，由 SessionController 处理
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

    // 自身加入时 room_joined 已携带完整名单，此处会重复；
    // MemberModel::append 对已存在的 clientId 直接忽略
    if (!m_members->append(member)) {
        LOG_DEBUG("Room") << "成员已存在，忽略重复的加入通知: " << member.nickname;
        return;
    }

    LOG_INFO("Room") << "成员加入: " << member.nickname
                     << (member.isHost ? "（房主）" : "");
    emit memberJoined(member.nickname);
}

void RoomSession::handleMemberLeft(const Protocol::Message &message) {
    const std::optional<Member> removed = m_members->remove(message.clientId);
    if (!removed) {
        LOG_DEBUG("Room") << "成员离开通知对应的成员不存在: clientId=" << message.clientId;
        return;
    }

    LOG_INFO("Room") << "成员离开: " << removed->nickname;
    emit memberLeft(removed->nickname);
}

void RoomSession::handleMemberOnline(const Protocol::Message &message) {
    if (!m_members->setOnline(message.clientId, true)) {
        LOG_DEBUG("Room") << "成员恢复在线通知对应的成员不存在: clientId="
                          << message.clientId;
        return;
    }

    LOG_INFO("Room") << "成员恢复在线: clientId=" << message.clientId;
    emit memberOnline(message.clientId);
}

void RoomSession::handlePlayback(const Protocol::Message &message) {
    emit playbackCommandReceived(actionFromWire(message.action), message.position,
                                 message.rate);
}

void RoomSession::handleVideoStatus(const Protocol::Message &message) {
    m_members->setVideoStatus(message.clientId, message.loaded, message.duration);

    // 条目时长在添加时未知，由加载它的成员一并告知列表。
    // video_status 不带 itemId，但只有当前条目会被加载，故对应到当前条目。
    if (message.loaded && message.duration > 0) {
        const QString itemId = m_playlist->currentItemId();
        if (!itemId.isEmpty())
            m_playlist->setDuration(itemId, message.duration);
    }
}

void RoomSession::handlePlaylistChanged(const Protocol::Message &message) {
    LOG_INFO("Room") << "播放列表更新: 条目数=" << message.playlist.size()
                     << " currentIndex=" << message.currentIndex;
    m_playlist->setEntries(message.playlist, message.currentIndex);
}

void RoomSession::handlePlaylistSwitched(const Protocol::Message &message) {
    m_playlist->setCurrentIndex(message.currentIndex);

    // 以 model 中的状态为准（itemId 与 currentIndex 一致）；
    // 列表为空时即为空串，下游据此卸载播放器
    emit playlistSwitched(m_playlist->currentItemId());
}

void RoomSession::handlePlaylistStatus(const Protocol::Message &message) {
    // 服务端已聚合好的各条匹配状态，直接铺到 model 上。
    // 记录日志是为了排查「该条为何显示未匹配」——仅看界面无法判断是谁尚未选文件
    for (const PlaylistEntry &entry : message.playlist)
        LOG_DEBUG("Playlist") << "匹配状态:" << entry.itemId.left(8) << "→" << entry.status;

    m_playlist->applyStatuses(message.playlist);
}

void RoomSession::handleVideoMismatch(const Protocol::Message &message) {
    if (message.videoMismatched == m_videoMismatched
        && message.shortestDuration == m_shortestDuration) {
        return;
    }

    LOG_INFO("Room") << "视频一致性变化: mismatched=" << (message.videoMismatched ? 1 : 0)
                     << " shortestDuration=" << message.shortestDuration;
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
    if (m_networkManager == nullptr) {
        LOG_WARN("Room") << "网络模块不可用，消息未发送";
        return false;
    }


    if (!m_networkManager->isConnected()) {
        LOG_DEBUG("Room") << "网络未连接，消息未发送";
        return false;
    }

    if (!m_networkManager->sendMessage(text)) {
        LOG_WARN("Room") << "消息发送失败（连接不可用）";
        return false;
    }
    return true;
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
    case PlaybackAction::Rate:
        return QStringLiteral("rate");
    }
    return QStringLiteral("seek");
}

RoomSession::PlaybackAction RoomSession::actionFromWire(const QString &action) {
    if (action == QLatin1String("play"))
        return PlaybackAction::Play;
    if (action == QLatin1String("pause"))
        return PlaybackAction::Pause;
    if (action == QLatin1String("rate"))
        return PlaybackAction::Rate;
    // 未知动作（例如更新版客户端的新命令）退化为 seek：老客户端收到 rate
    // 时只是跳一下位置，不会把播放状态弄乱
    return PlaybackAction::Seek;
}
