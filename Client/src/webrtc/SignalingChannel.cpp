//
// Created by donggu on 2026/9/13.
//

#include "SignalingChannel.h"

#include "core/NetworkManager.h"
#include "session/RoomSession.h"
#include "webrtc/WebrtcManager.h"

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

    // 出站:WebRTC 事件 -> 信令消息
    connect(m_webrtcManager, &WebrtcManager::offerCreated,
            this, &SignalingChannel::onOfferCreated);
    connect(m_webrtcManager, &WebrtcManager::answerCreated,
            this, &SignalingChannel::onAnswerCreated);
    connect(m_webrtcManager, &WebrtcManager::iceCandidateCreated,
            this, &SignalingChannel::onIceCandidateCreated);

    // 入站:信令消息 -> WebRTC 状态机
    connect(m_networkManager, &NetworkManager::messageReceived,
            this, &SignalingChannel::onMessage);

    // 离开房间后对端作废,下次共享重新解析
    connect(m_session, &RoomSession::left, this, &SignalingChannel::onRoomLeft);
}

// ============================
// 出站
// ============================

bool SignalingChannel::resolvePeer() {
    if (!m_peerId.isEmpty())
        return true;

    // 发起 offer 的一方此前没收到过对端消息,目标只能从房间成员里解析
    m_peerId = m_session->firstOtherMemberId();
    return !m_peerId.isEmpty();
}

void SignalingChannel::onOfferCreated(const QString &sdp) {
    if (!resolvePeer()) {
        emit errorOccurred("房间里没有其他成员,无法发起共享");
        return;
    }

    send(Protocol::encodeWebrtcOffer(m_peerId, sdp));
}

void SignalingChannel::onAnswerCreated(const QString &sdp) {
    if (!resolvePeer()) {
        emit errorOccurred("对端成员已离开");
        return;
    }

    send(Protocol::encodeWebrtcAnswer(m_peerId, sdp));
}

void SignalingChannel::onIceCandidateCreated(const QString &sdp,
                                             const QString &sdpMid,
                                             int sdpMLineIndex) {
    if (!resolvePeer())
        return;

    send(Protocol::encodeWebrtcIce(m_peerId, sdp, sdpMid, sdpMLineIndex));
}

// ============================
// 入站
// ============================

void SignalingChannel::onMessage(const QString &text) {
    const std::optional<Protocol::Message> message = Protocol::decode(text);
    if (!message)
        return;

    switch (message->type) {
    case Protocol::MessageType::WebrtcOffer:
        m_peerId = message->from;
        // offer 到达会自动触发内部 createAnswer
        m_webrtcManager->setRemoteDescription(message->sdp.toStdString(), "offer");
        break;

    case Protocol::MessageType::WebrtcAnswer:
        m_peerId = message->from;
        m_webrtcManager->setRemoteDescription(message->sdp.toStdString(), "answer");
        break;

    case Protocol::MessageType::WebrtcIce:
        m_webrtcManager->addIceCandidate(message->sdp.toStdString(),
                                         message->sdpMid.toStdString(),
                                         message->sdpMLineIndex);
        break;

    default:
        // 其余消息归 SessionController / RoomSession
        break;
    }
}

void SignalingChannel::onRoomLeft() {
    m_peerId.clear();
}

bool SignalingChannel::send(const QString &text) {
    if (m_session == nullptr || !m_session->inRoom())
        return false;

    return m_networkManager->sendMessage(text);
}
