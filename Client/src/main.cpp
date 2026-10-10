
#include <QDebug>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>
#include <QVideoFrame>

#include "rtc_base/logging.h"

#include "core/Log.h"
#include "core/NetworkManager.h"
#include "playback/PlaybackController.h"
#include "playback/PlaybackSync.h"
#include "session/RoomSession.h"
#include "session/SessionController.h"
#include "webrtc/SignalingChannel.h"
#include "webrtc/WebrtcManager.h"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);

    // 日志最先打开:后面每个对象构造时打的日志都要落进文件。
    // 默认输出到 ~/.local/share/Syncine/logs/,
    // 级别用 SYNCINE_LOG_LEVEL 调(trace/debug/info/warn/error),
    // 目录用 SYNCINE_LOG_DIR 覆盖。
    Log::init(QStringLiteral("syncine"));

    // 诊断开关:设了 SYNCINE_WEBRTC_LOG=1 才打开 WebRTC 自己的日志。
    //
    // 用来查"数据进了适配器却没发出去"这一类问题 —— WebRTC 内部很多失败
    // 是静默的(比如 SetSend 失败只在 LS_ERROR 级别留一行),平时不该刷屏,
    // 需要时再开。
    if (qEnvironmentVariableIsSet("SYNCINE_WEBRTC_LOG")) {
        webrtc::LogMessage::LogToDebug(webrtc::LS_INFO);
        webrtc::LogMessage::LogTimestamps(true);
    }

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

    // 发送:电影音频帧 → WebRTC 的第二条音轨。
    // 比视频那条多一步:WebRTC 要的是裸 PCM,而 QAudioBuffer 是带格式描述的容器,
    // 所以在这里把它拆成 指针 + 每声道采样数 + 采样率 + 声道数。
    QObject::connect(&playbackController, &PlaybackController::movieAudioFrameAvailable,
                     &app, [&webrtcManager](const QAudioBuffer &buffer) {
                         const QAudioFormat format = buffer.format();
                         webrtcManager.pushMovieAudioPcm(
                             buffer.constData<int16_t>(),
                             static_cast<size_t>(buffer.frameCount()),
                             format.sampleRate(),
                             static_cast<size_t>(format.channelCount()));
                     });

    // 电影音轨的开关:既要在播,又要在"共享"模式。
    auto updateMovieAudioEnabled = [&playbackController, &roomSession, &webrtcManager] {
        const bool sharing = roomSession.roomMode() == RoomSession::RoomMode::Share;
        webrtcManager.setMovieAudioEnabled(sharing && playbackController.playing());
    };

    QObject::connect(&playbackController, &PlaybackController::playingChanged,
                     &app, updateMovieAudioEnabled);
    QObject::connect(&roomSession, &RoomSession::roomModeChanged,
                     &app, updateMovieAudioEnabled);

    // 「电影音量」是**一个**值,要同时管住两个出声的地方:
    //
    //   本地模式 / 共享模式的房主 → 声音来自本机播放器(m_audioOutput)
    //   共享模式的观众           → 声音来自房主推过来的那条电影音轨(MovieAudioPlayer)
    //
    // 对用户来说都是"电影的音量",不该因为身处哪个模式就分成两个控件 ——
    // 界面上只有一个滑条(绑在 SignalingChannel.movieVolume 上),所以在这里
    // 把它扇出到本机播放器。缺了这行,本地模式下拖滑条是没反应的(听的是自己的播放器,
    // 而滑条改的是远端音轨那个音量)。
    auto applyMovieVolume = [&playbackController, &signalingChannel] {
        playbackController.setVolume(signalingChannel.movieVolume());
    };
    QObject::connect(&signalingChannel, &SignalingChannel::movieVolumeChanged,
                     &app, applyMovieVolume);
    applyMovieVolume(); // 启动先对齐一次(默认 1.0)

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
                         LOG_WARN("Signaling") << message;
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

    const int exitCode = app.exec();
    Log::shutdown();
    return exitCode;
}
