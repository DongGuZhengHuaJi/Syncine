//
// Created by donggu on 2026/9/13.
//

#include "SignalingChannel.h"

#include "core/Log.h"
#include "core/NetworkManager.h"
#include "session/RoomSession.h"
#include "webrtc/WebrtcManager.h"

namespace {

LinkKind linkKindOf(const QString &link) {
    return link == QLatin1String("media") ? LinkKind::Media : LinkKind::Voice;
}

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

    if (!m_webrtcManager->createPeerConnection(peerId))
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
        if (!ensurePeer(peerId))
            continue;

        if (!shouldInitiate(peerId))
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
    m_offeredLinks.remove(offerKey(peerId, linkKindId(LinkKind::Voice)));
    m_offeredLinks.remove(offerKey(peerId, linkKindId(LinkKind::Media)));
    if (m_webrtcManager != nullptr)
        m_webrtcManager->removePeer(peerId);
}

void SignalingChannel::onRoomEntered() {
    syncPeers();
}

void SignalingChannel::onMembersChanged() {
    if (m_session == nullptr || !m_session->inRoom())
        return;

    syncPeers();
}

void SignalingChannel::onRoomLeft() {
    m_offeredLinks.clear();
    if (m_webrtcManager != nullptr)
        m_webrtcManager->closeAllPeers();
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
    LOG_INFO("Signaling") << "与对端" << peerId << "的" << link << "连接已建立";
    emit peerConnected(peerId);
}

void SignalingChannel::onPeerClosed(const QString &peerId, const QString &link) {
    // 两条连接各自会发这个信号 —— 一条断了不代表对端没了，所以这里不
    // 直接对外报"对端断开",只记日志。真正判断对端是否还在，看成员表。
    LOG_DEBUG("Signaling") << "对端" << peerId << "的" << link << "连接已关闭";
}

void SignalingChannel::onPeerError(const QString &peerId, const QString &link,
                                   const QString &message) {
    // 连接级错误（ICE 失败、协商失败）不弹给用户：网状网里一个人掉线
    // 不该打断其他人。只报给上层，由它决定怎么处理。
    LOG_ERROR("Signaling") << "对端" << peerId << "的" << link << "连接出错:" << message;
    emit errorOccurred(message);
}

bool SignalingChannel::send(const QString &text) {
    if (m_session == nullptr || !m_session->inRoom())
        return false;

    return m_networkManager->sendMessage(text);
}
