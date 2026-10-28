
#include <memory>

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
#include "core/Log.h"
#include "core/NetworkManager.h"
#include "core/Toolchain.h"
#include "core/UrlDownloader.h"
#include "danmaku/DanmakuController.h"
#include "media/MediaPlatform.h"
#include "media/UrlInspector.h"
#include "playback/PlaybackController.h"
#include "playback/PlaybackSync.h"
#include "session/RoomSession.h"
#include "session/SessionController.h"
#include "transfer/FileTransferManager.h"
#include "version.h"
#include "webrtc/SignalingChannel.h"
#include "webrtc/WebrtcLog.h"
#include "webrtc/WebrtcManager.h"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);

    // 日志最先打开：后面每个对象构造时打的日志都要落进文件。
    // 默认输出到 ~/.local/share/Syncine/logs/,
    // 级别用 SYNCINE_LOG_LEVEL 调（trace/debug/info/warn/error）,
    // 目录用 SYNCINE_LOG_DIR 覆盖。
    Log::init(QStringLiteral("syncine"));
    // 联调时先确认两端跑的是不是同一份构建；版本号相同也可能来自不同提交。
    LOG_INFO("Log") << "Syncine" << SYNCINE_VERSION << "构建于"
                    << __DATE__ << __TIME__;

    // WebRTC 自己的日志一律接进日志文件（警告及以上），见 WebrtcLog.h 的说明。
    // 必须在 Log::init 之后：接收器直接写 Log。
    const bool webRtcVerbose = qEnvironmentVariableIsSet("SYNCINE_WEBRTC_LOG");
    WebrtcLog::install(webRtcVerbose);

    // 诊断开关：设了 SYNCINE_WEBRTC_LOG=1 再额外把它打到终端。
    //
    // 用来查"数据进了适配器却没发出去"这一类问题 —— WebRTC 内部很多失败
    // 是静默的（比如 SetSend 失败只在 LS_ERROR 级别留一行）,平时不该刷屏，
    // 需要时再开。日志文件那份由 WebrtcLog 负责，这里只管终端。
    if (webRtcVerbose) {
        webrtc::LogMessage::LogToDebug(webrtc::LS_INFO);
        webrtc::LogMessage::LogTimestamps(true);
    }

    // 应用对象放进内层作用域：它们析构时的收尾日志（尤其是 WebrtcManager
    // 停止 WebRTC 线程）仍然能落进文件。作用域结束后才关闭日志系统。
    int exitCode = 0;
    {
        // 全局设置（~/.config/Syncine/syncine.ini）。
        // **必须最先构造**:后面好几个对象在构造时就要读它(服务器地址、日志级别、
        // 音频设备),晚了就赶不上。
        AppSettings appSettings;
        appSettings.applyLogLevel();


        // ---- 基础设施 ----
        // 服务器地址从设置里来（留空 = 用 AppSettings 里的兜底地址）,
        // 真正的"试着连、连不上换兜底、都失败就重试"在 NetworkManager 里
        NetworkManager networkManager(&appSettings);
        PlaybackController playbackController;
        WebrtcManager webrtcManager;

        // 外部命令行工具（yt-dlp / ffmpeg）的定位与自检。只在网链模式用得上，
        // 但启动时就把结果记进日志 —— "为什么下载失败"最常见的原因就是它俩没就位。
        Toolchain toolchain;

        // 网链模式的平台注册表：URL 匹配、B 站编号展开、cookie 策略。
        // QML 通过 mediaPlatforms 调 normalize/detect。
        PlatformBridge platformBridge;

        // cookie:给 yt-dlp 带上登录态（从浏览器导出 / 手动导入）。
        // 必须在 Toolchain 之后（要用它的 yt-dlp 路径）、在两个下载器之前
        CookieManager cookieManager(&toolchain, &appSettings);

        // ---- 房间与会话 ----
        RoomSession roomSession(&networkManager);
        SessionController sessionController(&networkManager, &roomSession);

        // ---- 网链模式：把链接变成可播放的媒体 ----
        // 两个对象互不干扰：playbackDownloader 跟随房间当前条目（由 PlaybackSync 驱动）；
        // urlInspector 供弹窗解析链接（标题/画质/流列表）。共用一个会导致解析互相阻塞。
        UrlDownloader playbackDownloader(&toolchain, &cookieManager);
        UrlInspector urlInspector(&toolchain, &cookieManager);

        // HDR 是下载端的硬约束：设置关闭时只选 SDR，下载后再用 ffprobe 复核。
        // 不再做离线转码；设置变化立刻作用于下一次解析。
        playbackDownloader.setAllowHdr(appSettings.hdrAllowed());
        QObject::connect(&appSettings, &AppSettings::hdrAllowedChanged, &playbackDownloader,
                         [&appSettings, &playbackDownloader]() {
                             playbackDownloader.setAllowHdr(appSettings.hdrAllowed());
                         });

        // 通用链接的 cookie 策略：Bilibili 始终带，Generic 由设置决定。
        playbackDownloader.setGenericCookies(appSettings.genericCookies());
        urlInspector.setGenericCookies(appSettings.genericCookies());
        QObject::connect(&appSettings, &AppSettings::genericCookiesChanged, &app,
                         [&appSettings, &playbackDownloader, &urlInspector]() {
                             playbackDownloader.setGenericCookies(
                                 appSettings.genericCookies());
                             urlInspector.setGenericCookies(
                                 appSettings.genericCookies());
                         });

        // 网链模式的 B 站弹幕：用 yt-dlp 的 danmaku 字幕轨下载 XML，
        // 再按播放进度供 QML 取条目。声明在 PlaybackSync 之前。
        DanmakuController danmakuController(&toolchain, &cookieManager);

        // ---- 播放同步：唯一同时持有「房间」与「播放器」的地方 ----
        PlaybackSync playbackSync(&roomSession, &playbackController, &playbackDownloader,
                                  &danmakuController);

        // ---- WebRTC 信令：房间 <-> WebRTC 状态机 ----
        SignalingChannel signalingChannel(&roomSession, &networkManager, &webrtcManager);

        // ---- 文件传输：走 voice 连接上已建立的数据通道 ----
        // 状态全部保存在这里，QML 的传输窗口只是视图，关掉不影响任务
        FileTransferManager fileTransferManager(&webrtcManager, &roomSession);

        // 文件传输进行中时给共享视频封一个较低的码率上限，传输结束再恢复。
        // 只影响房主的 media 连接；观众端没有发送流，调用是空操作。
        QObject::connect(&fileTransferManager,
                         &FileTransferManager::transferActivityChanged,
                         &webrtcManager,
                         [&webrtcManager](bool active) {
                             webrtcManager.setTransferActive(active);
                         });

        // ICE 配置不在此处提供：它随房间下发（服务端在 room_created / room_joined 中
        // 给出，含 Cloudflare 兑换的临时 TURN 凭据），由 SignalingChannel 在每次建连前
        // 交给 WebrtcManager，从而更换服务端或 TURN 供应商都无需修改客户端。
        // 此处只初始化线程与工厂，对端连接由 SignalingChannel 在进房后按成员表创建。
        webrtcManager.initialize();

        // ---- 音频设备（麦克风 / 听筒）装配 ----
        // 集中在这一个装配点的原因：
        //   1. 界面修改设备 → 落盘。
        //   2. 听筒是「所有出声的地方共用一个选择」，需扇出到分属两套设备体系的
        //      三个对象：对方语音与推来的电影声走 WebrtcManager（后者由它持有的
        //      MovieAudioPlayer 播放，同一信号在 setDesiredDevice 中一并切换）；
        //      本机播放的电影声需在此接线（见下方 lambda），二者互不相识。
        //   3. 启动时将已存下的偏好写入一次，否则重启即丢失。
        // 顺序要求：先接线，再写入启动偏好。顺序颠倒时启动那次 setSpeakerDevice
        // 会在无监听者的情况下发出信号——WebrtcManager 内部仍生效，但本机播放器
        // 收不到通知，「上次选的耳机」只生效一半。
        QObject::connect(&signalingChannel, &SignalingChannel::micDeviceChanged, &app,
                         [&appSettings](const QString &name) {
                             appSettings.setMicDevice(name);
                         });
        QObject::connect(&signalingChannel, &SignalingChannel::speakerDeviceChanged, &app,
                         [&appSettings, &playbackController](const QString &name) {
                             appSettings.setSpeakerDevice(name);
                             // 设备名（ADM 给的 PulseAudio 描述）→ Qt 的输出设备
                             playbackController.setOutputDevice(findAudioOutputByName(name));
                         });

        signalingChannel.setMicDevice(appSettings.micDevice());
        signalingChannel.setSpeakerDevice(appSettings.speakerDevice());

        // ---- 视频的两条接线 ----
        // 两侧接口有意互不相识：PlaybackController 只负责发出「解出一帧」，
        // WebrtcManager 只负责收帧并送出/渲染，胶水贴在这唯一的装配点上。

        // 发送：本地解码帧 → WebRTC（含 Qt 格式 → I420 的转换）。
        // 按模式门控：仅共享模式推视频。其他模式下各端看的是自己的本地文件，
        // 这一路推出无人渲染（接收端会丢弃），而发送端仍需付出逐行搬运、编码与上行带宽。
        // 门控挡在 push 之前而非仅关轨道，是为了把 Qt 帧 → I420 的逐行转换开销一并省掉；
        // 轨道开关见下方 updateVideoEnabled。
        QObject::connect(&playbackController, &PlaybackController::videoFrameAvailable,
                         &app, [&webrtcManager, &roomSession](const QVideoFrame &f) {
                             // 只有共享模式的房主推视频；观众端即使本地有残留
                             // 播放器也不该占上行。
                             if (roomSession.roomMode() != RoomSession::RoomMode::Share
                                 || !roomSession.isHost())
                                 return;
                             webrtcManager.pushQtVideoFrame(f);
                         });

        // 发送：电影音频帧 → WebRTC 的第二条音轨。
        // 比视频多一步：WebRTC 需要裸 PCM，而 QAudioBuffer 是带格式描述的容器，
        // 故在此拆为指针 + 每声道采样数 + 采样率 + 声道数。
        QObject::connect(&playbackController, &PlaybackController::movieAudioFrameAvailable,
                         &app, [&webrtcManager, &roomSession](const QAudioBuffer &buffer) {
                             // 只有共享模式的房主需要电影声；观众端即使在共享模式
                             // 也没有本地媒体，但门禁放在这里可以省掉无意义的 PCM 搬运。
                             if (roomSession.roomMode() != RoomSession::RoomMode::Share
                                 || !roomSession.isHost())
                                 return;
                             const QAudioFormat format = buffer.format();
                             webrtcManager.pushMovieAudioPcm(
                                 buffer.constData<int16_t>(),
                                 static_cast<size_t>(buffer.frameCount()),
                                 format.sampleRate(),
                                 static_cast<size_t>(format.channelCount()));
                         });

        // 电影音轨的两级开关：
        //   发送层（active）：只有「共享模式 + 房主」才真正发 RTP。观众端和非共享
        //                     模式直接停发，不再为没人听的音轨持续付静音包/编码成本。
        //   静音层（enabled）：共享模式下暂停时只静音、不停流，恢复播放没有
        //                     启动延迟（停流再启动要重新搭编码/打包链路）。
        auto updateMovieAudioEnabled = [&playbackController, &roomSession, &webrtcManager] {
            const bool hostSharing =
                roomSession.roomMode() == RoomSession::RoomMode::Share
                && roomSession.isHost();
            webrtcManager.setMovieAudioEnabled(hostSharing && playbackController.playing());
            webrtcManager.setMovieAudioSending(hostSharing);
        };

        QObject::connect(&playbackController, &PlaybackController::playingChanged,
                         &app, updateMovieAudioEnabled);
        QObject::connect(&roomSession, &RoomSession::roomModeChanged,
                         &app, updateMovieAudioEnabled);
        QObject::connect(&roomSession, &RoomSession::isHostChanged,
                         &app, updateMovieAudioEnabled);
        updateMovieAudioEnabled(); // 启动时对齐一次（默认同步模式 → 停发）

        // 视频轨开关：同理，只有「共享模式 + 房主」才发。
        // 注意 WebRTC 语义：set_enabled(false) 并非停发，而是改发黑帧以保持流不断；
        // setLocalVideoSending(false) 才真正把发送流 Stop 掉，再配合上方停止喂帧。
        auto updateVideoEnabled = [&roomSession, &webrtcManager] {
            const bool hostSharing =
                roomSession.roomMode() == RoomSession::RoomMode::Share
                && roomSession.isHost();
            webrtcManager.setLocalVideoEnabled(hostSharing);
            webrtcManager.setLocalVideoSending(hostSharing);
        };

        QObject::connect(&roomSession, &RoomSession::roomModeChanged,
                         &app, updateVideoEnabled);
        QObject::connect(&roomSession, &RoomSession::isHostChanged,
                         &app, updateVideoEnabled);
        updateVideoEnabled(); // 启动时对齐一次（默认同步模式 → 关）

        // 「电影音量」是单一取值，需同时管住两个出声位置：
        //   同步模式 / 共享模式的房主 → 声音来自本机播放器（m_audioOutput）
        //   共享模式的观众           → 声音来自房主推来的电影音轨（MovieAudioPlayer）
        // 对用户而言都是「电影的音量」，不应因模式不同而拆成两个控件；界面上只有一个
        // 滑条（绑在 SignalingChannel.movieVolume 上），故在此扇出到本机播放器。
        // 缺少此行时，同步模式下拖动滑条无效果（听到的是本机播放器，而滑条修改的是
        // 远端音轨的音量）。
        auto applyMovieVolume = [&playbackController, &signalingChannel] {
            playbackController.setVolume(signalingChannel.movieVolume());
        };
        QObject::connect(&signalingChannel, &SignalingChannel::movieVolumeChanged,
                         &app, applyMovieVolume);
        applyMovieVolume(); // 启动先对齐一次(默认 1.0)

        // 接收：将渲染器的落点交由 PlaybackController 保管。
        // 不能在此直接调用 setRemoteVideoSink(playbackController.displayVideoSink())：
        // 此时 engine 尚未 load，QML 的 VideoOutput 不存在，必然取得 null，
        // 导致渲染器永远向空指针推帧（静默丢弃）。改由 PlaybackController 在 QML
        // 交出 sink 后再转给渲染器（见 bindVideoOutput）。
        playbackController.setRemoteRenderer(&webrtcManager);

        // 观众端收到远端 seek：清掉电影声缓冲（含 NetEq 丢弃窗口）和视频待显示队列。
        QObject::connect(&playbackSync, &PlaybackSync::remoteSeeked,
                         &webrtcManager, [&webrtcManager](qint64) {
                             webrtcManager.flushRemotePlayback();
                         });

        QObject::connect(&signalingChannel, &SignalingChannel::errorOccurred, &app,
                         [](const QString &message) {
                             LOG_WARN("Signaling") << message;
                         });


        // engine 必须**先于日志系统**析构：QML 引擎拆除过程中还会产生告警，
        // 那些行应该还能落进日志文件。
        //
        // 用 unique_ptr 而不是栈对象，是为了能在 return 之前显式 reset ——
        // 写成 `QQmlApplicationEngine engine;` 的话它要等函数返回才析构，
        // 那时 Log::shutdown() 早就执行过了，退出阶段的告警既进不了文件、
        // 也看不到（GUI 程序没有可看 stderr 的终端）。
        auto engine = std::make_unique<QQmlApplicationEngine>();
        engine->rootContext()->setContextProperty("appSettings", &appSettings);
        engine->rootContext()->setContextProperty("networkManager", &networkManager);
        engine->rootContext()->setContextProperty("playbackController", &playbackController);
        engine->rootContext()->setContextProperty("roomSession", &roomSession);
        engine->rootContext()->setContextProperty("sessionController", &sessionController);
        engine->rootContext()->setContextProperty("playbackSync", &playbackSync);
        engine->rootContext()->setContextProperty("danmakuController", &danmakuController);
        engine->rootContext()->setContextProperty("signalingChannel", &signalingChannel);
        engine->rootContext()->setContextProperty("fileTransferManager", &fileTransferManager);
        // 界面用的探测器：添加条目时解析链接（标题/时长/画质档位/流列表），只解析不下载；
        // 实际下载走 playbackSync 那条线
        engine->rootContext()->setContextProperty("urlInspector", &urlInspector);
        engine->rootContext()->setContextProperty("cookieManager", &cookieManager);
        engine->rootContext()->setContextProperty("mediaPlatforms", &platformBridge);

        // 外部工具链（yt-dlp / ffmpeg）自检。版本探测是异步的（yt-dlp 为 PyInstaller
        // 打包，启动需数百毫秒），不会阻塞界面。
        QTimer::singleShot(1200, &toolchain, &Toolchain::detect);
        engine->rootContext()->setContextProperty("toolchain", &toolchain);

        // 装配完成、设置已读取，此时才发起连接（NetworkManager 不在构造中连接，
        // 否则会早于设置）。连接是异步的，无需等待。
        networkManager.connectToServer();

        engine->load(QUrl(QStringLiteral(
            "qrc:/qt/qml/SyncineApp/ui/Main.qml"
        )));

        exitCode = app.exec();

        // 退出前退一次房：临时文件（下载合并出来的视频）在这里整体清掉 ——
        // **这是缓存唯一的删除时机**(切条目只停下载、不删文件，下载好的切回来直接复用)。
        // 关闭窗口时 RoomPage 只是被销毁，不会发出 leave_room，故必须在此显式调用；
        // 同时让服务端立即感知本端离开，无需等待心跳超时。
        sessionController.leaveRoom();

        // 顺序要紧：先拆 QML 引擎（此间的告警仍会进日志），再让应用对象析构
        engine.reset();
    } // 应用对象在此析构：WebRTC 停线程等收尾日志仍然写得到文件

    Log::shutdown();
    return exitCode;
}
