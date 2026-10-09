//
// Created by donggu on 2026/9/13.
//

#include "SignalingChannel.h"

#include <iostream>

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


    // ---- 入站:信令消息 -> WebRTC 状态机 ----
    connect(m_networkManager, &NetworkManager::messageReceived,
            this, &SignalingChannel::onMessage);

    // ---- 房间生命周期 -> 连接的建与拆 ----
    connect(m_session, &RoomSession::entered, this, &SignalingChannel::onRoomEntered);
    connect(m_session, &RoomSession::left, this, &SignalingChannel::onRoomLeft);

    // 成员一变就重新对齐。成员表是"谁该连着"的唯一事实来源,
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
}

// ============================
// 麦克风
// ============================

void SignalingChannel::setAudioEnabled(bool enabled) {
    if (m_webrtcManager == nullptr)
        return;

    std::cout << "[SignalingChannel] setAudioEnabled: " << (enabled ? "true" : "false") << std::endl;
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

double SignalingChannel::chatVolume() const {
    return m_webrtcManager != nullptr ? m_webrtcManager->remoteChatVolume() : 1.0;
}

void SignalingChannel::setChatVolume(double volume) {
    if (m_webrtcManager == nullptr)
        return;

    m_webrtcManager->setRemoteChatVolume(volume);
}

// ============================
// 连接的对齐:该建谁、该删谁
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

    if (m_webrtcManager->peer(peerId, LinkKind::Voice) != nullptr)
        return true;

    if (!m_webrtcManager->createPeerConnection(peerId))
        return false;

    // 一个对端现在有两条连接,各自独立协商,所以要分别监听它们的信号。
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

        // 两条连接各自独立发起 —— 它们是两次完全独立的 SDP 协商,
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
// 出站:WebRTC -> 信令
// ============================
//
// 每条消息都带 peerId(编码进 "to" 字段),服务端据此定向转发,
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
// 入站:信令 -> WebRTC
// ============================
//
// 每条消息的 from 字段就是这条信令属于哪个对端。
// 路由不需要状态,从消息本身读出来即可 —— 这也是为什么
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
        // 死锁的破解就在这一步(见 shouldInitiate 的说明)。
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
            // 收到了一个我们不认识的 answer —— 说明它对应的 offer 不是我们发的,
            // 或者连接已经被拆了。静默忽略,不新建连接(否则会凭空多出一条)。
            std::cerr << "[信令] 忽略来自未知连接的回答: " << from.toStdString()
                      << ":" << message->link.toStdString() << std::endl;
            return;
        }

        link->setRemoteDescription(message->sdp, QStringLiteral("answer"));
        break;
    }

    case Protocol::MessageType::WebrtcIce: {
        const QString from = message->from;
        if (from.isEmpty())
            return;

        // ICE 与 SDP 是两条独立通道,候选可能先于 offer 到达。
        // 这里若连接还不存在就先丢掉 —— 重新收集的候选随后就到,
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
    std::cout << "[信令] 与对端 " << peerId.toStdString()
              << " 的 " << link.toStdString() << " 连接已建立" << std::endl;
    emit peerConnected(peerId);
}

void SignalingChannel::onPeerClosed(const QString &peerId, const QString &link) {
    // 两条连接各自会发这个信号 —— 一条断了不代表对端没了,所以这里不
    // 直接对外报"对端断开",只记日志。真正判断对端是否还在,看成员表。
    std::cout << "[信令] 对端 " << peerId.toStdString()
              << " 的 " << link.toStdString() << " 连接已关闭" << std::endl;
}

void SignalingChannel::onPeerError(const QString &peerId, const QString &link,
                                   const QString &message) {
    // 连接级错误(ICE 失败、协商失败)不弹给用户:网状网里一个人掉线
    // 不该打断其他人。只报给上层,由它决定怎么处理。
    std::cerr << "[信令] 对端 " << peerId.toStdString() << " 的 "
              << link.toStdString() << " 连接出错: "
              << message.toStdString() << std::endl;
    emit errorOccurred(message);
}

bool SignalingChannel::send(const QString &text) {
    if (m_session == nullptr || !m_session->inRoom())
        return false;

    return m_networkManager->sendMessage(text);
}
