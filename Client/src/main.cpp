
#include <QDebug>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>
#include <QVideoFrame>

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

    // ---- 视频的两条接线 ----
    //
    // 两边接口故意互不认识:PlaybackController 只管发"解出一帧了",
    // WebrtcManager 只管收帧并送出/渲染。胶水贴在这唯一一个装配点上。

    // 发送:本地解码帧 → WebRTC(会做 Qt 格式 → I420 的转换)
    QObject::connect(&playbackController, &PlaybackController::videoFrameAvailable,
                     &app, [&webrtcManager](const QVideoFrame &f) {
                         webrtcManager.pushQtVideoFrame(f);
                     });

    // 接收:把渲染器的落点交给 PlaybackController 保管。
    //
    // **不能**在这里直接 setRemoteVideoSink(playbackController.displayVideoSink()) ——
    // 此刻 engine 还没 load,QML 的 VideoOutput 还不存在,拿到的必然是 null,
    // 结果是渲染器永远对着空指针推帧(静默丢弃)。
    //
    // 改由 PlaybackController 在 QML 交出 sink 后再转给渲染器(见 bindVideoOutput)。
    playbackController.setRemoteRenderer(&webrtcManager);

    QObject::connect(&signalingChannel, &SignalingChannel::errorOccurred, &app,
                     [](const QString &message) {
                         qWarning() << "信令:" << message;
                     });

    engine.rootContext()->setContextProperty("networkManager", &networkManager);
    engine.rootContext()->setContextProperty("playbackController", &playbackController);
    engine.rootContext()->setContextProperty("roomSession", &roomSession);
    engine.rootContext()->setContextProperty("sessionController", &sessionController);
    engine.rootContext()->setContextProperty("playbackSync", &playbackSync);
    engine.rootContext()->setContextProperty("signalingChannel", &signalingChannel);

    engine.load(QUrl(QStringLiteral(
        "qrc:/qt/qml/SyncineApp/ui/Main.qml"
    )));

    return app.exec();
}
