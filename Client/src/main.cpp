#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "PlaybackController.h"
#include "NetworkManager.h"
#include "RoomManager.h"
#include "WebrtcManager.h"

int main(int argc, char *argv[]) {
    QGuiApplication a(argc, argv);

    QQmlApplicationEngine engine;

    PlaybackController playbackController;
    NetworkManager networkManager;
    RoomManager roomManager;
    WebrtcManager webrtcManager;

    roomManager.setNetworkManager(&networkManager);
    roomManager.setPlaybackController(&playbackController);

    // 局域网联调暂不需要 STUN;上互联网时再配置
    if (webrtcManager.initialize({})) {
        webrtcManager.createPeerConnection();
    }

    // v1 只支持一个对端(单观众);多人共享时需要为每个观众各建一个 WebrtcManager
    QString webrtcPeerId;

    // 出站:WebRTC 事件 → 信令消息,经服务器定向转发
    QObject::connect(&webrtcManager, &WebrtcManager::offerCreated, &a,
                     [&roomManager, &webrtcPeerId](const QString &sdp) {
        roomManager.sendWebrtcOffer(webrtcPeerId, sdp);
    });
    QObject::connect(&webrtcManager, &WebrtcManager::answerCreated, &a,
                     [&roomManager, &webrtcPeerId](const QString &sdp) {
        roomManager.sendWebrtcAnswer(webrtcPeerId, sdp);
    });
    QObject::connect(&webrtcManager, &WebrtcManager::iceCandidateCreated, &a,
                     [&roomManager, &webrtcPeerId](const QString &sdp,
                                                   const QString &sdpMid,
                                                   int sdpMLineIndex) {
        roomManager.sendWebrtcIce(webrtcPeerId, sdp, sdpMid, sdpMLineIndex);
    });

    // 入站:信令消息 → WebRTC 状态机(offer 会自动触发 createAnswer)
    QObject::connect(&roomManager, &RoomManager::webrtcOfferReceived, &a,
                     [&webrtcManager, &webrtcPeerId](const QString &from,
                                                     const QString &sdp) {
        webrtcPeerId = from;
        webrtcManager.setRemoteDescription(sdp.toStdString(), "offer");
    });
    QObject::connect(&roomManager, &RoomManager::webrtcAnswerReceived, &a,
                     [&webrtcManager, &webrtcPeerId](const QString &from,
                                                     const QString &sdp) {
        webrtcPeerId = from;
        webrtcManager.setRemoteDescription(sdp.toStdString(), "answer");
    });
    QObject::connect(&roomManager, &RoomManager::webrtcIceReceived, &a,
                     [&webrtcManager](const QString &,
                                      const QString &sdp,
                                      const QString &sdpMid,
                                      int sdpMLineIndex) {
        webrtcManager.addIceCandidate(sdp.toStdString(),
                                      sdpMid.toStdString(),
                                      sdpMLineIndex);
    });

    engine.rootContext()->setContextProperty("playbackController", & playbackController);
    engine.rootContext()->setContextProperty("networkManager", & networkManager);
    engine.rootContext()->setContextProperty("roomManager", & roomManager);

    engine.load(QUrl(QStringLiteral(
    "qrc:/qt/qml/SyncineApp/ui/Main.qml"
    )));


    return a.exec();
}
