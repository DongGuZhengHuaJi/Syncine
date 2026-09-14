
#include <QDebug>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>

#include "core/NetworkManager.h"
#include "playback/PlaybackController.h"
#include "playback/PlaybackSync.h"
#include "session/RoomSession.h"
#include "session/SessionController.h"
#include "webrtc/SignalingChannel.h"
#include "webrtc/WebrtcManager.h"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;

    // ---- 基础设施 ----
    NetworkManager networkManager;
    PlaybackController playbackController;
    WebrtcManager webrtcManager;

    // ---- 房间与会话 ----
    RoomSession roomSession(&networkManager);
    SessionController sessionController(&networkManager, &roomSession);

    // ---- 播放同步:唯一同时持有「房间」和「播放器」的地方 ----
    PlaybackSync playbackSync(&roomSession, &playbackController);

    // ---- WebRTC 信令:房间 <-> WebRTC 状态机 ----
    SignalingChannel signalingChannel(&roomSession, &networkManager, &webrtcManager);

    // 局域网联调暂不需要 STUN;上互联网时再配置。
    //
    // 这里只初始化线程和工厂 —— 对端连接**不在这里建**。
    // 每条连接对应一个对端,由 SignalingChannel 在进房后按成员表创建。
    webrtcManager.initialize({});

    QObject::connect(&signalingChannel, &SignalingChannel::errorOccurred, &app,
                     [](const QString &message) {
                         qWarning() << "信令:" << message;
                     });

    engine.rootContext()->setContextProperty("networkManager", &networkManager);
    engine.rootContext()->setContextProperty("playbackController", &playbackController);
    engine.rootContext()->setContextProperty("roomSession", &roomSession);
    engine.rootContext()->setContextProperty("sessionController", &sessionController);
    engine.rootContext()->setContextProperty("playbackSync", &playbackSync);

    engine.load(QUrl(QStringLiteral(
        "qrc:/qt/qml/SyncineApp/ui/Main.qml"
    )));

    return app.exec();
}
