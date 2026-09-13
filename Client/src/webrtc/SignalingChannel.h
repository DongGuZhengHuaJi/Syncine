//
// WebRTC 信令 <-> 房间消息 之间的胶水层。
//
// 这些逻辑原先散在 main.cpp 里:6 条 connect、一个 webrtcPeerId 成员、
// 以及"offer 的接收方还没定下来时从成员列表里猜一个"这种业务流程。
// 它属于「共享模式会话」的业务,不属于「程序启动」,所以单独成类,
// main.cpp 于是退回成纯粹的装配点。
//
// 出站:WebRTC 事件 -> 信令消息
// 入站:信令消息 -> WebRTC 状态机
//

#ifndef SYNCINE_SIGNALINGCHANNEL_H
#define SYNCINE_SIGNALINGCHANNEL_H

#include <QObject>
#include <QString>

#include "core/Protocol.h"

class NetworkManager;
class RoomSession;
class WebrtcManager;

class SignalingChannel : public QObject {
    Q_OBJECT

public:
    SignalingChannel(RoomSession *session,
                     NetworkManager *networkManager,
                     WebrtcManager *webrtcManager,
                     QObject *parent = nullptr);
    ~SignalingChannel() override = default;

signals:
    void errorOccurred(const QString &message);

private slots:
    void onMessage(const QString &text);

    void onOfferCreated(const QString &sdp);
    void onAnswerCreated(const QString &sdp);
    void onIceCandidateCreated(const QString &sdp,
                               const QString &sdpMid,
                               int sdpMLineIndex);

    void onRoomLeft();

private:
    bool send(const QString &text);
    // 记录对端;返回 false 表示这次没能确定对端(房间里没有别人)
    bool resolvePeer();

    RoomSession *m_session = nullptr;
    NetworkManager *m_networkManager = nullptr;
    WebrtcManager *m_webrtcManager = nullptr;

    // v1 只支持单对端(单观众);更多人共享时需要为每个观众各建一个 WebrtcManager
    QString m_peerId;
};

#endif //SYNCINE_SIGNALINGCHANNEL_H
