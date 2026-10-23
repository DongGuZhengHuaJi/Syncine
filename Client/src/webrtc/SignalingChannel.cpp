//
// Created by donggu on 2026/9/13.
//

#include "SignalingChannel.h"

#include <QTimer>

#include "core/Log.h"
#include "core/NetworkManager.h"
#include "session/RoomSession.h"
#include "webrtc/WebrtcManager.h"

namespace {

LinkKind linkKindOf(const QString &link) {
    return link == QLatin1String("media") ? LinkKind::Media : LinkKind::Voice;
}

// ICE 恢复策略：
//   · Disconnected 先等 5 秒，期间自行恢复就取消；
//   · Failed 或宽限期结束才真正发起 restart；
//   · 每次重试间隔 2s → 5s → 10s，最多 3 次；
//   · restartIce() 返回 false 说明协商正忙，2 秒后再试，不计入重试次数。
constexpr int kIceGraceMs = 5000;
constexpr int kMaxIceRestarts = 3;
constexpr int kIceRestartDelaysMs[] = {2000, 5000, 10000};
constexpr int kIceBusyRetryMs = 2000;
constexpr int kMaxIceBusyRetries = 5;

} // namespace

SignalingChannel::SignalingChannel(RoomSession *session,
                                   NetworkManager *networkManager,
                                   WebrtcManager *webrtcManager,
                                   QObject *parent)
    : QObject(parent),
      m_session(session),
      m_networkManager(networkManager),
      m_webrtcManager(webrtcManager) {

    if (m_session == nullptr || m_networkManager == nullptr || m_webrtcManager == nullptr)
        return;


    // ---- 入站：信令消息 -> WebRTC 状态机 ----
    connect(m_networkManager, &NetworkManager::messageReceived,
            this, &SignalingChannel::onMessage);

    // ---- 房间生命周期 -> 连接的建与拆 ----
    connect(m_session, &RoomSession::entered, this, &SignalingChannel::onRoomEntered);
    connect(m_session, &RoomSession::left, this, &SignalingChannel::onRoomLeft);

    // 成员一变就重新对齐。成员表是"谁该连着"的唯一事实来源，
    // 分开监听 memberJoined / memberLeft 会漏掉 room_joined 时的整表下发。
    connect(m_session, &RoomSession::membersChanged,
            this, &SignalingChannel::onMembersChanged);

    connect(m_session, &RoomSession::memberOnline,
            this, &SignalingChannel::onMemberOnline);

    // clientId 变化意味着对端看到的已经是另一个成员：旧 id 的 PeerConnection
    // 在对端那边已经被拆掉，本地也必须一起拆，等 entered 按新 id 重建。
    connect(m_session, &RoomSession::clientIdChanged,
            this, &SignalingChannel::onClientIdChanged);

    // 转发 WebrtcManager 的状态信号给 QML,不要自己存状态。
    connect(m_webrtcManager, &WebrtcManager::audioEnabledChanged,
            this, &SignalingChannel::audioEnabledChanged);
    connect(m_webrtcManager, &WebrtcManager::remoteMovieVolumeChanged,
            this, &SignalingChannel::movieVolumeChanged);
    connect(m_webrtcManager, &WebrtcManager::remoteChatVolumeChanged,
            this, &SignalingChannel::chatVolumeChanged);

    connect(m_webrtcManager, &WebrtcManager::videoQualityChanged, this,
            [this](WebrtcManager::VideoQuality quality) {
                emit videoQualityChanged(static_cast<int>(quality));
            });

    // 音频设备：列表变了两个属性都要重算(QML 一次只看得见一个列表，
    // 但分开成两个信号没有意义 —— 枚举本来就不分侧)
    connect(m_webrtcManager, &WebrtcManager::audioDevicesChanged, this, [this] {
        emit micDevicesChanged();
        emit speakerDevicesChanged();
    });
    connect(m_webrtcManager, &WebrtcManager::recordingDeviceChanged,
            this, &SignalingChannel::micDeviceChanged);
    connect(m_webrtcManager, &WebrtcManager::playoutDeviceChanged,
            this, &SignalingChannel::speakerDeviceChanged);
}

// ============================
// 麦克风
// ============================

void SignalingChannel::setAudioEnabled(bool enabled) {
    if (m_webrtcManager == nullptr)
        return;

    LOG_DEBUG("Signaling") << "setAudioEnabled:" << enabled;
    m_webrtcManager->setAudioEnabled(enabled);
}

bool SignalingChannel::audioEnabled() const {
    return m_webrtcManager != nullptr && m_webrtcManager->audioEnabled();
}

// ============================
// 接收端音量
// ============================

double SignalingChannel::movieVolume() const {
    return m_webrtcManager != nullptr ? m_webrtcManager->remoteMovieVolume() : 1.0;
}

void SignalingChannel::setMovieVolume(double volume) {
    if (m_webrtcManager == nullptr)
        return;

    m_webrtcManager->setRemoteMovieVolume(volume);
}

int SignalingChannel::videoQuality() const {
    return m_webrtcManager != nullptr
               ? static_cast<int>(m_webrtcManager->videoQuality())
               : 3; // 原画
}

void SignalingChannel::setVideoQuality(int quality) {
    if (m_webrtcManager == nullptr)
        return;

    const int clamped = qBound(0, quality, 3);
    m_webrtcManager->setVideoQuality(
        static_cast<WebrtcManager::VideoQuality>(clamped));
}

double SignalingChannel::chatVolume() const {
    return m_webrtcManager != nullptr ? m_webrtcManager->remoteChatVolume() : 1.0;
}

void SignalingChannel::setChatVolume(double volume) {
    if (m_webrtcManager == nullptr)
        return;

    m_webrtcManager->setRemoteChatVolume(volume);
}

// ============================
// 音频设备（麦克风 / 听筒）
// ============================

void SignalingChannel::refreshAudioDevices() {
    if (m_webrtcManager == nullptr)
        return;

    m_webrtcManager->refreshAudioDevices();
}

void SignalingChannel::setMicDevice(const QString &name) {
    if (m_webrtcManager == nullptr)
        return;

    // 值没变就什么都不做。启动时 main.cpp 会把存下来的偏好喂进来一次，
    // 若是空串（从没选过）这里就静悄悄地过去 —— 否则每次启动都会白白
    // 把声卡拉起来，还会在日志里留一行看着像"用户切换了设备"的记录。
    if (m_webrtcManager->recordingDeviceName() == name)
        return;

    LOG_DEBUG("Signaling") << "切换麦克风设备:" << (name.isEmpty() ? QStringLiteral("(系统默认)") : name);
    m_webrtcManager->selectRecordingDevice(name);
}

void SignalingChannel::setSpeakerDevice(const QString &name) {
    if (m_webrtcManager == nullptr)
        return;

    if (m_webrtcManager->playoutDeviceName() == name)
        return;

    LOG_DEBUG("Signaling") << "切换听筒设备:" << (name.isEmpty() ? QStringLiteral("(系统默认)") : name);
    m_webrtcManager->selectPlayoutDevice(name);
}

QVariantList SignalingChannel::micDevices() const {
    return m_webrtcManager != nullptr ? m_webrtcManager->recordingDevices() : QVariantList{};
}

QVariantList SignalingChannel::speakerDevices() const {
    return m_webrtcManager != nullptr ? m_webrtcManager->playoutDevices() : QVariantList{};
}

QString SignalingChannel::micDevice() const {
    return m_webrtcManager != nullptr ? m_webrtcManager->recordingDeviceName() : QString();
}

QString SignalingChannel::speakerDevice() const {
    return m_webrtcManager != nullptr ? m_webrtcManager->playoutDeviceName() : QString();
}

// ============================
// 连接的对齐：该建谁、该删谁
// ============================

bool SignalingChannel::shouldInitiate(const QString &peerId) const {
    if (m_session == nullptr)
        return false;

    return m_session->clientId() < peerId;
}

QString SignalingChannel::offerKey(const QString &peerId, const QString &link) {
    return peerId + QLatin1Char('|') + link;
}

bool SignalingChannel::ensurePeer(const QString &peerId) {
    if (m_webrtcManager == nullptr || peerId.isEmpty())
        return false;

    // 建连前把当前的 ICE 配置（STUN/TURN）交给 WebrtcManager。
    //
    // 放这里而不是 syncPeers():ensurePeer 是**唯一**的建连入口 ——
    // syncPeers() 走它，收到 offer 的 onMessage() 也直接走它(那条路根本不经过
    // syncPeers),放别处会漏。QList 隐式共享，这个赋值是 O(1);
    // 配置只在 CreatePeerConnection 那一刻生效，已连上的对端不受影响。
    if (m_session != nullptr)
        m_webrtcManager->setIceServers(m_session->iceServers());

    if (m_webrtcManager->peer(peerId, LinkKind::Voice) != nullptr)
        return true;

    // 数据通道只在发起 offer 的一侧创建（见 WebrtcManager::createPeerConnection）。
    // shouldInitiate 在建连双方给出的答案相反，正好保证只有一侧创建。
    if (!m_webrtcManager->createPeerConnection(peerId, shouldInitiate(peerId)))
        return false;

    // 一个对端现在有两条连接，各自独立协商，所以要分别监听它们的信号。
    for (LinkKind kind : {LinkKind::Voice, LinkKind::Media}) {
        PeerLink *link = m_webrtcManager->peer(peerId, kind);
        if (link == nullptr)
            return false;

        connect(link, &PeerLink::offerCreated,
                this, &SignalingChannel::onOfferCreated);
        connect(link, &PeerLink::answerCreated,
                this, &SignalingChannel::onAnswerCreated);
        connect(link, &PeerLink::iceCandidateCreated,
                this, &SignalingChannel::onIceCandidateCreated);
        connect(link, &PeerLink::connected,
                this, &SignalingChannel::onPeerConnected);
        connect(link, &PeerLink::closed,
                this, &SignalingChannel::onPeerClosed);
        connect(link, &PeerLink::errorOccurred,
                this, &SignalingChannel::onPeerError);
        connect(link, &PeerLink::iceDisconnected,
                this, &SignalingChannel::onIceDisconnected);
        connect(link, &PeerLink::iceFailed,
                this, &SignalingChannel::onIceFailed);
    }

    return true;
}

void SignalingChannel::syncPeers() {
    if (m_session == nullptr || m_webrtcManager == nullptr)
        return;

    const QList<QString> want = m_session->otherMemberIds();

    // 删除不在表中的连接
    const QList<QString> existing = m_webrtcManager->peerIds();
    for (const QString &peerId : existing) {
        if (!want.contains(peerId))
            teardownPeer(peerId);
    }

    // 建立新连接
    for (const QString &peerId : want) {
        const bool online = m_session->isMemberOnline(peerId);
        const bool hasConnection =
            m_webrtcManager->peer(peerId, LinkKind::Voice) != nullptr;

        // 对方断线等待重连：已有连接先留着（P2P 可能还活着），
        // 但不要新建，也不要往一个离线成员发 offer。
        if (!online && !hasConnection) {
            LOG_DEBUG("Signaling") << "成员离线等待重连,暂不建连:" << peerId;
            continue;
        }

        if (!ensurePeer(peerId))
            continue;

        if (!online || !shouldInitiate(peerId))
            continue;

        // 两条连接各自独立发起 —— 它们是两次完全独立的 SDP 协商，
        // 各有各的 offer/answer/ICE,状态也互不相干。
        for (LinkKind kind : {LinkKind::Voice, LinkKind::Media}) {
            const QString key = offerKey(peerId, linkKindId(kind));
            if (m_offeredLinks.contains(key))
                continue;

            m_offeredLinks.insert(key);
            if (auto *link = m_webrtcManager->peer(peerId, kind))
                link->createOffer();
        }
    }
}

void SignalingChannel::teardownPeer(const QString &peerId) {
    for (const QString &link : {linkKindId(LinkKind::Voice),
                                linkKindId(LinkKind::Media)}) {
        m_offeredLinks.remove(offerKey(peerId, link));
        clearIceRecovery(peerId, link);
    }
    if (m_webrtcManager != nullptr)
        m_webrtcManager->removePeer(peerId);
}

void SignalingChannel::onRoomEntered() {
    syncPeers();
    // 信令断线期间 ICE restart 的 offer 发不出去；resume 成功后重新排一次。
    rearmIceRecovery();
}

void SignalingChannel::onMembersChanged() {
    if (m_session == nullptr || !m_session->inRoom())
        return;

    syncPeers();
}

void SignalingChannel::onMemberOnline(const QString &clientId) {
    LOG_INFO("Signaling") << "成员恢复在线,重新对齐连接:" << clientId;
    syncPeers();
    // 对端离线期间，ICE restart 的 offer 会被服务端判成 member_not_found；
    // 现在它回来了，重新给满重试次数。
    rearmIceRecovery();
}

void SignalingChannel::onRoomLeft() {
    m_offeredLinks.clear();
    m_iceRestartAttempts.clear();
    m_iceBusyRetries.clear();
    m_iceGraceGeneration.clear();
    m_iceFailedLinks.clear();
    if (m_webrtcManager != nullptr)
        m_webrtcManager->closeAllPeers();
}

void SignalingChannel::onClientIdChanged() {
    // 首次进房时没有连接可拆；断线重连/换房时这里必须清空，
    // 否则会继续用旧 id 的 PeerConnection 跟同一个对端通信。
    LOG_DEBUG("Signaling") << "本端 clientId 变化，重置所有对端连接";
    onRoomLeft();
}

// ============================
// 出站：WebRTC -> 信令
// ============================
//
// 每条消息都带 peerId(编码进 "to" 字段),服务端据此定向转发，
// 不再需要"猜对端是谁"。

void SignalingChannel::onOfferCreated(const QString &peerId, const QString &link,
                                      const QString &sdp) {
    send(Protocol::encodeWebrtcOffer(peerId, link, sdp));
}

void SignalingChannel::onAnswerCreated(const QString &peerId, const QString &link,
                                       const QString &sdp) {
    send(Protocol::encodeWebrtcAnswer(peerId, link, sdp));
}

void SignalingChannel::onIceCandidateCreated(const QString &peerId,
                                             const QString &link,
                                             const QString &sdp,
                                             const QString &sdpMid,
                                             int sdpMLineIndex) {
    send(Protocol::encodeWebrtcIce(peerId, link, sdp, sdpMid, sdpMLineIndex));
}

// ============================
// 入站：信令 -> WebRTC
// ============================
//
// 每条消息的 from 字段就是这条信令属于哪个对端。
// 路由不需要状态，从消息本身读出来即可 —— 这也是为什么
// SignalingChannel 里没有任何"当前对端"之类的成员。

void SignalingChannel::onMessage(const QString &text) {
    const std::optional<Protocol::Message> message = Protocol::decode(text);
    if (!message)
        return;

    switch (message->type) {
    case Protocol::MessageType::WebrtcOffer: {
        const QString from = message->from;
        if (from.isEmpty())
            return;

        // 对端先发起了。即使按规则"该我发起",也照样收下 ——
        // 死锁的破解就在这一步（见 shouldInitiate 的说明）。
        if (!ensurePeer(from))
            return;

        auto *link = m_webrtcManager->peer(from, linkKindOf(message->link));
        if (link == nullptr)
            return;

        link->setRemoteDescription(message->sdp, QStringLiteral("offer"));
        break;
    }

    case Protocol::MessageType::WebrtcAnswer: {
        const QString from = message->from;
        if (from.isEmpty())
            return;

        auto *link = m_webrtcManager->peer(from, linkKindOf(message->link));
        if (link == nullptr) {
            // 收到无法识别的 answer——说明其对应的 offer 并非本端发出，
            // 或连接已被拆除。静默忽略，不新建连接（否则会凭空产生一条）。
            LOG_WARN("Signaling") << "忽略来自未知连接的回答:" << from << ":" << message->link;
            return;
        }

        link->setRemoteDescription(message->sdp, QStringLiteral("answer"));
        break;
    }

    case Protocol::MessageType::WebrtcIce: {
        const QString from = message->from;
        if (from.isEmpty())
            return;

        // ICE 与 SDP 是两条独立通道，候选可能先于 offer 到达。
        // 这里若连接还不存在就先丢掉 —— 重新收集的候选随后就到，
        // 而为一条可能永远不来的 offer 提前建连接是更糟的赌注。
        auto *link = m_webrtcManager->peer(from, linkKindOf(message->link));
        if (link == nullptr)
            return;

        link->addIceCandidate(message->sdp, message->sdpMid, message->sdpMLineIndex);
        break;
    }

    default:
        // 其余消息归 SessionController / RoomSession
        break;
    }
}

// ============================
// 连接状态
// ============================

void SignalingChannel::onPeerConnected(const QString &peerId, const QString &link) {
    // 连接后清除 ICE 恢复状态
    clearIceRecovery(peerId, link);
    LOG_INFO("Signaling") << "与对端" << peerId << "的" << link << "连接已建立";
    emit peerConnected(peerId);
}

void SignalingChannel::onPeerClosed(const QString &peerId, const QString &link) {
    // 两条连接各自会发这个信号 —— 一条断了不代表对端没了，所以这里不
    // 直接对外报"对端断开",只记日志。真正判断对端是否还在，看成员表。
    clearIceRecovery(peerId, link);
    LOG_DEBUG("Signaling") << "对端" << peerId << "的" << link << "连接已关闭";
}

void SignalingChannel::onPeerError(const QString &peerId, const QString &link,
                                   const QString &message) {
    // 连接级错误（协商失败等）不弹给用户：网状网里一个人掉线
    // 不该打断其他人。只报给上层，由它决定怎么处理。
    LOG_ERROR("Signaling") << "对端" << peerId << "的" << link << "连接出错:" << message;
    emit errorOccurred(message);
}

// ============================
// ICE 恢复
// ============================
//
// 三条约束决定了这里的形状：
//   1. 只有 shouldInitiate() 一侧能发 offer，否则双方同时 restart 会撞 glare；
//   2. OnIceConnectionChange 运行在 WebRTC 信令线程，QTimer/状态操作必须
//      回到主线程 —— 这里靠 PeerLink 的信号 + 本对象住主线程自动排队；
//   3. Disconnected 可能自己恢复，Failed 不会，所以两者用不同的等待时间。

void SignalingChannel::onIceDisconnected(const QString &peerId, const QString &link) {
    // 应答方不重启，等发起方的新 offer。两边都重启会各发一份 offer。
    if (!shouldInitiate(peerId))
        return;

    const QString key = offerKey(peerId, link);
    const quint64 generation = ++m_iceGenerationCounter;
    m_iceGraceGeneration[key] = generation;

    LOG_DEBUG("Signaling") << "ICE 断开,进入" << kIceGraceMs / 1000 << "秒宽限期:"
                           << peerId << link;
    QTimer::singleShot(kIceGraceMs, this, [this, peerId, link, key, generation] {
        // 宽限期内恢复成 Connected 会移除这个 key；代次不匹配说明已经恢复，
        // 或者已经有更新的重启请求在排队。
        if (m_iceGraceGeneration.value(key) != generation)
            return;
        LOG_WARN("Signaling") << "ICE 宽限期结束仍未恢复,发起 restart:" << peerId << link;
        scheduleIceRestart(peerId, link, 0, /*countAttempt=*/true);
    });
}

void SignalingChannel::onIceFailed(const QString &peerId, const QString &link) {
    if (!shouldInitiate(peerId))
        return;

    // 记下来：如果此时信令也断了，restart offer 发不出去；
    // room resumed 后 rearmIceRecovery() 会重新尝试。
    m_iceFailedLinks.insert(offerKey(peerId, link));
    LOG_WARN("Signaling") << "ICE 失败,立即尝试 restart:" << peerId << link;
    scheduleIceRestart(peerId, link, 0, /*countAttempt=*/true);
}

void SignalingChannel::scheduleIceRestart(const QString &peerId, const QString &link,
                                          int delayMs, bool countAttempt) {
    if (m_session == nullptr || m_webrtcManager == nullptr)
        return;
    if (!shouldInitiate(peerId))
        return;

    const QString key = offerKey(peerId, link);

    // 新的重启请求作废尚未到期的宽限期计时器。
    m_iceGraceGeneration.remove(key);

    const int attempt = m_iceRestartAttempts.value(key, 0);
    if (countAttempt && attempt >= kMaxIceRestarts) {
        LOG_ERROR("Signaling") << "ICE restart 连续失败,放弃:" << peerId << link;
        emit errorOccurred(QStringLiteral("与 %1 的连接无法恢复").arg(peerId));
        return;
    }

    const int delay = delayMs > 0
        ? delayMs
        : kIceRestartDelaysMs[qBound(0, attempt, kMaxIceRestarts - 1)];

    QTimer::singleShot(delay, this, [this, peerId, link, key, countAttempt] {
        if (m_session == nullptr || !m_session->inRoom())
            return;

        // 信令还没恢复：offer 发不出去。不要消耗重试次数，
        // 保留失败标记，等 room resumed 后的 rearm 再来。
        if (m_networkManager == nullptr || !m_networkManager->isConnected()) {
            m_iceFailedLinks.insert(key);
            return;
        }

        PeerLink *peer = m_webrtcManager->peer(peerId, linkKindOf(link));
        if (peer == nullptr)
            return;

        const int current = m_iceRestartAttempts.value(key, 0);
        if (countAttempt && current >= kMaxIceRestarts) {
            LOG_ERROR("Signaling") << "ICE restart 连续失败,放弃:" << peerId << link;
            emit errorOccurred(QStringLiteral("与 %1 的连接无法恢复").arg(peerId));
            return;
        }

        if (!peer->restartIce()) {
            // 上一次 offer/answer 还没结束。不要把它算成一次失败的重启，
            // 过 2 秒再试；连续撞上多次说明协商状态卡住了，别无限循环。
            if (m_iceBusyRetries.value(key, 0) >= kMaxIceBusyRetries) {
                LOG_ERROR("Signaling") << "协商状态卡住，ICE restart 放弃:"
                                       << peerId << link;
                emit errorOccurred(QStringLiteral("与 %1 的连接无法恢复").arg(peerId));
                return;
            }
            ++m_iceBusyRetries[key];
            scheduleIceRestart(peerId, link, kIceBusyRetryMs, /*countAttempt=*/false);
            return;
        }

        if (countAttempt)
            m_iceRestartAttempts[key] = current + 1;
        m_iceBusyRetries.remove(key);
        LOG_INFO("Signaling") << "已发起 ICE restart:" << peerId << link
                              << "attempt=" << m_iceRestartAttempts.value(key);
    });
}

void SignalingChannel::rearmIceRecovery() {
    if (m_session == nullptr || m_webrtcManager == nullptr)
        return;

    const QList<QString> failed = m_iceFailedLinks.values();
    for (const QString &key : failed) {
        const int separator = key.indexOf(QLatin1Char('|'));
        if (separator <= 0)
            continue;
        const QString peerId = key.left(separator);
        const QString link = key.mid(separator + 1);
        if (m_webrtcManager->peer(peerId, linkKindOf(link)) == nullptr)
            continue;

        // 断线期间那些重启尝试根本没有发出去，重新给满次数。
        m_iceRestartAttempts.remove(key);
        m_iceBusyRetries.remove(key);
        LOG_INFO("Signaling") << "信令已恢复,重新尝试 ICE restart:" << peerId << link;
        scheduleIceRestart(peerId, link, 0, /*countAttempt=*/true);
    }
}

void SignalingChannel::clearIceRecovery(const QString &peerId, const QString &link) {
    const QString key = offerKey(peerId, link);
    // 移除代次即可让尚未到期的宽限期计时器醒来后直接退出。
    m_iceGraceGeneration.remove(key);
    m_iceRestartAttempts.remove(key);
    m_iceBusyRetries.remove(key);
    m_iceFailedLinks.remove(key);
}

bool SignalingChannel::send(const QString &text) {
    if (m_session == nullptr || !m_session->inRoom())
        return false;

    return m_networkManager->sendMessage(text);
}
