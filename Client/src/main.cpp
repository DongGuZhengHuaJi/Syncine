
#include <QDebug>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>

#include "rtc_base/logging.h"

#include "core/AppSettings.h"
#include "core/AudioDevices.h"
#include "core/CookieManager.h"
#include "core/HdrConverter.h"
#include "core/Log.h"
#include "core/NetworkManager.h"
#include "core/Toolchain.h"
#include "core/UrlDownloader.h"
#include "media/UrlInspector.h"
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

    // 全局设置(~/.config/Syncine/syncine.ini)。
    // **必须最先构造**:后面好几个对象在构造时就要读它(服务器地址、日志级别、
    // 音频设备),晚了就赶不上。
    AppSettings appSettings;
    appSettings.applyLogLevel();


    // ---- 基础设施 ----
    // 服务器地址从设置里来(留空 = 用 AppSettings 里的兜底地址),
    // 真正的"试着连、连不上换兜底、都失败就重试"在 NetworkManager 里
    NetworkManager networkManager(&appSettings);
    PlaybackController playbackController;
    WebrtcManager webrtcManager;

    // 外部命令行工具(yt-dlp / ffmpeg)的定位与自检。只在网链模式用得上,
    // 但启动时就把结果记进日志 —— "为什么下载失败"最常见的原因就是它俩没就位。
    Toolchain toolchain;

    // cookie:给 yt-dlp 带上登录态(从浏览器导出 / 手动导入)。
    // 必须在 Toolchain 之后(要用它的 yt-dlp 路径)、在两个下载器之前
    CookieManager cookieManager(&toolchain, &appSettings);

    // HDR → SDR:检测到 HDR 视频时提示用户转码(Qt 的渲染不做色调映射,会过曝)
    HdrConverter hdrConverter(&toolchain);

    // ---- 房间与会话 ----
    RoomSession roomSession(&networkManager);
    SessionController sessionController(&networkManager, &roomSession);

    // ---- 网链模式:把链接变成能播的媒体 ----
    //
    // 两个对象,互不干扰:
    //   playbackDownloader —— 跟着房间的"当前条目"走,由 PlaybackSync 驱动
    //   urlInspector       —— 加条目时"粘贴链接 → 看标题/画质/流列表"用,弹窗直接调
    // 共用一个的话,一边在解析时另一边就被挡掉了。
    UrlDownloader playbackDownloader(&toolchain, &cookieManager);
    UrlInspector urlInspector(&toolchain, &cookieManager);

    // ---- 播放同步:唯一同时持有「房间」和「播放器」的地方 ----
    PlaybackSync playbackSync(&roomSession, &playbackController, &playbackDownloader);

    // ---- WebRTC 信令:房间 <-> WebRTC 状态机 ----
    SignalingChannel signalingChannel(&roomSession, &networkManager, &webrtcManager);

    // ICE 配置(STUN/TURN)不在这里给:它跟着房间走 —— 服务端在 room_created /
    // room_joined 里下发(含从 Cloudflare 换来的临时 TURN 凭据,API token 只在
    // 服务端),由 SignalingChannel 在每次建连前交给 WebrtcManager。
    // 好处是换服务端、换 TURN 供应商都不用改客户端。
    //
    // 这里只初始化线程和工厂 —— 对端连接**不在这里建**。
    // 每条连接对应一个对端,由 SignalingChannel 在进房后按成员表创建。
    webrtcManager.initialize();

    // ---- 音频设备(麦克风 / 听筒)的装配 ----
    //
    // 三件事,都放在这一个装配点上(而不是塞进 SignalingChannel):
    //
    //   1. 界面改了设备 → 落盘。
    //   2. **听筒是"所有出声的地方共用一个选择"**。这句注释不是修辞,
    //      它要扇到三个对象上,而它们分属两套设备体系:
    //        对方语音         → WebrtcManager(WebRTC 的 ADM,认索引)
    //        推过来的电影声    → WebrtcManager(它持有 MovieAudioPlayer,
    //                          同一个信号在 setDesiredDevice 里顺手换掉)
    //        本机播放的电影声  → 这里补一刀,见下面那个 lambda
    //      所以本机播放器和"听筒"之间这条线必须在这里接 —— 它们互不认识。
    //   3. 启动时把**存下来的**偏好喂进去一次,否则重启就丢。
    //
    // **顺序要紧**:先把线接好,再喂启动偏好。反过来的话,启动那次
    // setSpeakerDevice 会在没人监听的时候把信号发掉 —— WebrtcManager 内部
    // 照样生效,但本机播放器收不到通知,于是"上次选的耳机"只对了一半。
    QObject::connect(&signalingChannel, &SignalingChannel::micDeviceChanged, &app,
                     [&appSettings](const QString &name) {
                         appSettings.setMicDevice(name);
                     });
    QObject::connect(&signalingChannel, &SignalingChannel::speakerDeviceChanged, &app,
                     [&appSettings, &playbackController](const QString &name) {
                         appSettings.setSpeakerDevice(name);
                         // 设备名(ADM 给的 PulseAudio 描述)→ Qt 的输出设备
                         playbackController.setOutputDevice(findAudioOutputByName(name));
                     });

    signalingChannel.setMicDevice(appSettings.micDevice());
    signalingChannel.setSpeakerDevice(appSettings.speakerDevice());

    // ---- 视频的两条接线 ----
    //
    // 两边接口故意互不认识:PlaybackController 只管发"解出一帧了",
    // WebrtcManager 只管收帧并送出/渲染。胶水贴在这唯一一个装配点上。

    // 发送:本地解码帧 → WebRTC(会做 Qt 格式 → I420 的转换)
    //
    // **按模式门控**:只有共享模式才推视频。同步/网链模式下每个人看的都是
    // 自己的本地文件,这一路推出去没人会渲染(接收端会直接丢弃),
    // 但发送端已经付了"逐行搬运 + 编码 + 上行带宽"的钱。
    //
    // 挡在 push 之前而不是只关轨道,是因为 Qt 帧 → I420 那次逐行转换是纯 CPU
    // 开销,放在门控里面一起省掉。轨道本身的开关见下面的 updateVideoEnabled。
    // 加载本地文件后查一下是不是 HDR —— 是的话界面会提示"要不要转成 SDR"。
    // Qt 的渲染不做 HDR→SDR 映射,直接播会亮处过曝(详见 HdrConverter 顶部说明)。
    QObject::connect(&playbackController, &PlaybackController::sourceChanged, &app,
                     [&playbackController, &hdrConverter]() {
                         hdrConverter.check(playbackController.source());
                     });

    QObject::connect(&playbackController, &PlaybackController::videoFrameAvailable,
                     &app, [&webrtcManager, &roomSession](const QVideoFrame &f) {
                         if (roomSession.roomMode() != RoomSession::RoomMode::Share)
                             return;
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

    // 视频轨的开关:同理,只在共享模式下开着。
    //
    // 注意 WebRTC 的语义:set_enabled(false) 不是"什么都不发",而是改发黑帧
    // (保持流不断,接收端不会以为流结束了)。真正省下带宽的是上面那处
    // "不再喂帧"——两件事配合才是完整的门控。
    auto updateVideoEnabled = [&roomSession, &webrtcManager] {
        webrtcManager.setLocalVideoEnabled(
            roomSession.roomMode() == RoomSession::RoomMode::Share);
    };

    QObject::connect(&roomSession, &RoomSession::roomModeChanged,
                     &app, updateVideoEnabled);
    updateVideoEnabled(); // 启动时对齐一次(默认同步模式 → 关)

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


    // 先析构engine避免输出null错误日志
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("appSettings", &appSettings);
    engine.rootContext()->setContextProperty("networkManager", &networkManager);
    engine.rootContext()->setContextProperty("playbackController", &playbackController);
    engine.rootContext()->setContextProperty("roomSession", &roomSession);
    engine.rootContext()->setContextProperty("sessionController", &sessionController);
    engine.rootContext()->setContextProperty("playbackSync", &playbackSync);
    engine.rootContext()->setContextProperty("signalingChannel", &signalingChannel);
    // 界面用的探测器:加条目时解析链接(标题/时长/画质档位/流列表)。
    // 它只"看"不下载 —— 真正的下载走 playbackSync 那条线
    engine.rootContext()->setContextProperty("urlInspector", &urlInspector);
    engine.rootContext()->setContextProperty("cookieManager", &cookieManager);
    engine.rootContext()->setContextProperty("hdrConverter", &hdrConverter);

    // 外部工具链(yt-dlp / ffmpeg)。先让它去自检 —— 版本探测是异步的
    // (yt-dlp 是 PyInstaller 包,启动要几百毫秒),不会卡住界面。
    QTimer::singleShot(1200, &toolchain, &Toolchain::detect);
    engine.rootContext()->setContextProperty("toolchain", &toolchain);

    // 装配完了、设置也读过了,现在才发起连接(NetworkManager 自己不再
    // 在构造里连 —— 那样会赶在设置之前)。连接是异步的,不用等它。
    networkManager.connectToServer();

    engine.load(QUrl(QStringLiteral(
        "qrc:/qt/qml/SyncineApp/ui/Main.qml"
    )));

    const int exitCode = app.exec();

    // 退出前退一次房:临时文件(下载合并出来的视频)在这里整体清掉 ——
    // **这是缓存唯一的删除时机**(切条目只停下载、不删文件,下载好的切回来直接复用)。
    // 关窗口时 RoomPage 只是被销毁,不会发 leave_room,所以必须在这里显式调;
    // 顺带也让服务端立刻知道我们走了,不用等心跳超时。
    sessionController.leaveRoom();

    Log::shutdown();
    return exitCode;
}
