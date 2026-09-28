//
// WebRTC 信令 <-> 房间消息 之间的胶水层。
//
// 出站:WebRTC 事件 -> 带路由信息的信令消息
// 入站:信令消息 -> 对应 PeerLink 的状态机
//
// 多对端之后这一层多了两件它该管的事:
//   1. **该和谁建连** —— 谁发起、什么时候发起(syncPeers)
//   2. **路由** —— 每条信令归哪个对端(靠 peerId 认领)
//
// 为什么要定"谁发起":网状网里若两端同时 CreateOffer,会撞成 glare
// (双方都在 offer 状态,收到对方的 offer 谁也不知道该回滚谁)。
// 用 clientId 比较来单向决定,是无需额外协商就能打破对称的办法。
//

#ifndef SYNCINE_SIGNALINGCHANNEL_H
#define SYNCINE_SIGNALINGCHANNEL_H

#include <QObject>
#include <QSet>
#include <QString>

#include "core/Protocol.h"
#include "webrtc/PeerLink.h"

class NetworkManager;
class RoomSession;
class WebrtcManager;

class SignalingChannel : public QObject {
    Q_OBJECT

    // 麦克风开关状态。QML 直接读这个属性,不要自己再存一份。
    Q_PROPERTY(bool audioEnabled
               READ audioEnabled
               NOTIFY audioEnabledChanged)

public:
    SignalingChannel(RoomSession *session,
                     NetworkManager *networkManager,
                     WebrtcManager *webrtcManager,
                     QObject *parent = nullptr);
    ~SignalingChannel() override = default;

    // 开关麦克风。QML 绑定这个。
    Q_INVOKABLE void setAudioEnabled(bool enabled);

    bool audioEnabled() const;

signals:
    void errorOccurred(const QString &message);
    // 与某个对端的连接真正打通(ICE 完成),供界面显示状态
    void peerConnected(const QString &peerId);
    void peerDisconnected(const QString &peerId);
    // 麦克风实际开关状态。QML 用它驱动按钮显示,不要自己记状态
    void audioEnabledChanged(bool enabled);

private slots:
    void onMessage(const QString &text);

    void onOfferCreated(const QString &peerId, const QString &sdp);
    void onAnswerCreated(const QString &peerId, const QString &sdp);
    void onIceCandidateCreated(const QString &peerId,
                               const QString &sdp,
                               const QString &sdpMid,
                               int sdpMLineIndex);

    void onPeerConnected(const QString &peerId);
    void onPeerClosed(const QString &peerId);
    void onPeerError(const QString &peerId, const QString &message);

    void onRoomEntered();
    void onRoomLeft();
    void onMembersChanged();

private:
    // 房间成员表变化后,对齐连接:该建的建、该删的删
    void syncPeers();
    void teardownPeer(const QString &peerId);
    // 保证 peerId 有 PeerLink。已存在返回 true;新建失败返回 false。
    bool ensurePeer(const QString &peerId);
    // 本端是否该主动向 peerId 发起协商(半开连接的处理见 .cpp)
    bool shouldInitiate(const QString &peerId) const;
    bool send(const QString &text);

    RoomSession *m_session = nullptr;
    NetworkManager *m_networkManager = nullptr;
    WebrtcManager *m_webrtcManager = nullptr;

    // 已经发出过 offer 的对端。用来区分"等待连上"和"该重试"——
    // 没有它就无法判断一条半开的连接是死了还是还在握手中。
    QSet<QString> m_offeredPeers;
};

#endif //SYNCINE_SIGNALINGCHANNEL_H
