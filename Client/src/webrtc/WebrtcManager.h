//
// WebRTC 管理者：线程、编解码工厂，以及所有对端连接。
//
// 本类只持有全局唯一的部分（线程、PeerConnectionFactory、peerId → {voice, media}
// 对端表），自身不参与 SDP 协商；单条连接的协商状态机在 PeerLink，
// 因为协商状态按连接独立，上提到本类会成为共享变量而相互干扰。
//
// 每个对端使用两条 PeerConnection，两个工厂（不可合并）：
//   ADM 会把采集到的声音广播给同一工厂中所有已启动的发送流
//   （见 audio_state.cc 的 UpdateAudioTransportWithSendingStreams），
//   故语音与电影音轨同工厂会导致麦克风声音灌入电影音轨、电影音轨被两个线程
//   同时喂入而触发 AudioSendStream::SendAudioData 的 race 断言。
//   AudioState 为每个工厂一份，故必须使用另一个工厂：
//     工厂甲（真声卡）→ voice 连接：只挂麦克风音轨
//     工厂乙（假声卡）→ media 连接：挂视频轨 + 电影音轨
//   假声卡（kDummyAudio）不采集也不放音，广播线上无内容可灌；代价是观众端
//   电影声须由 MovieAudioPlayer 播放。两条连接各自独立协商，信令以 link 区分。
//

#ifndef SYNCINE_WEBRTCMANAGER_H
#define SYNCINE_WEBRTCMANAGER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <string>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/environment/environment.h"
#include "api/peer_connection_interface.h"
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/media_stream_interface.h"
#include "rtc_base/thread.h"

#include "core/RoomTypes.h"

#include "MovieAudioSource.h"
#include "PeerLink.h"
#include "RemoteVideoRenderer.h"
#include "VideoTrackSource.h"

class QVideoFrame;
class QVideoSink;
class MovieAudioPlayer;

class WebrtcManager : public QObject {
    Q_OBJECT

public:
    WebrtcManager();
    ~WebrtcManager() override;

    WebrtcManager(const WebrtcManager &) = delete;
    WebrtcManager &operator=(const WebrtcManager &) = delete;

    // 初始化两套线程与编解码工厂。可重复调用，第二次直接返回 true。
    //
    // ICE 配置不由参数提供——它随房间下发：服务端在 room_created / room_joined
    // 中给出（含 Cloudflare 兑换的临时 TURN 凭据），由 SignalingChannel 在建连前
    // 交给 setIceServers()。
    bool initialize();

    // ---- ICE 配置（STUN / TURN）----
    // 服务端下发的列表（见 RoomTypes.h 的 IceServerConfig）。必须在
    // createPeerConnection() 之前调用：配置在 CreatePeerConnection 时拷入
    // PeerConnection，WebRTC 无事后修改接口。只影响之后新建的连接；凭据有效期
    // 24 小时，libwebrtc 的 TurnPort 会周期性续租，过期后连接降级断开——
    // 热刷新（服务端推送 + SetConfiguration）本期未实现。
    // 线程：只在 Qt 主线程调用（createPeerConnection 亦然），故不加锁。
    void setIceServers(const QList<IceServerConfig> &servers);

    QList<IceServerConfig> iceServers() const {
        return m_iceServers;
    }

    // ---- 对端管理 ----

    // 为 peerId 创建两条连接（voice + media）。已存在则直接返回 true（幂等）。
    // createDataChannel 目前无作用
    bool createPeerConnection(const QString &peerId, bool createDataChannel = true);

    // 关闭并销毁某对端的两条连接。对端离开房间时调用。
    void removePeer(const QString &peerId);

    // 关闭所有对端（离开房间时调用）。线程与工厂保留，下次进房继续使用。
    void closeAllPeers();

    // 取某条连接，kind 决定取哪一条
    PeerLink *peer(const QString &peerId, LinkKind kind) const;

    // 当前所有对端 ID（不区分连接）。SignalingChannel 据此比对成员表。
    QList<QString> peerIds() const {
        return m_peers.keys();
    }

    bool isInitialized() const {
        return m_initialized;
    }

    bool hasPeerConnection() const {
        return !m_peers.isEmpty();
    }

    int peerCount() const {
        return static_cast<int>(m_peers.size());
    }

    // ---- 视频 ----

    // 视频源（帧的入口）。接上播放器后由 Qt 侧喂入。
    VideoTrackSource *videoSource() const {
        return m_videoSource.get();
    }

    // 将一帧送入 WebRTC。无人订阅时为空操作。
    void pushVideoFrame(const webrtc::VideoFrame &frame);

    // 实际入口：喂入一帧 Qt 解码出的画面。
    // 由 PlaybackController 的 videoFrameAvailable 信号驱动（接线在 main.cpp）。
    void pushQtVideoFrame(const QVideoFrame &frame);

    // 远端画面的输出目标，由 main.cpp 装配时设置。
    // 未设置时收到的远端视频会被丢弃（PeerLink 中会记录日志说明）。
    void setRemoteVideoSink(QVideoSink *sink);

    // 渲染器是否向 sink 写帧。
    //
    // 仅在「显示远端」时开启：显示本地时渲染器收到的是回声帧，不应渲染，
    // 否则会覆盖本地画面并维持回声环（见 PlaybackController::applyVideoSink）。
    void setRemoteVideoEnabled(bool enabled);

    // ---- 推送端视频 ----
    // 视频轨与电影音轨一样在 initialize() 预创建、建连时立即挂上，以保证其恒定
    // 存在于 SDP 中（加轨会触发重新协商，而 PeerLink 只处理初始协商）。
    // 非共享模式用 setLocalVideoEnabled(false) 关闭，无需改动 SDP。
    // 注意：VideoTrack::set_enabled(false) 并非停止发送，而是改发黑帧以保持流不断，
    // 真正节省带宽的是上游停止喂帧（见 main.cpp 中按模式门控的 pushQtVideoFrame）。
    void setLocalVideoEnabled(bool enabled);

    bool localVideoEnabled() const {
        return m_localVideoTrack != nullptr && m_localVideoTrack->enabled();
    }

    // 推送画质档位（共享模式下由房主设置）
    enum class VideoQuality {
        Smooth,   // 流畅
        Standard, // 标准
        Hd,       // 高清
        Original, // 原画
    };
    Q_ENUM(VideoQuality)

    void setVideoQuality(VideoQuality quality);

    VideoQuality videoQuality() const {
        return m_videoQuality;
    }

    // ---- 电影音频 ----
    // 数据并非来自声卡，而是 PlaybackController 经 QAudioBufferOutput 从
    // QMediaPlayer 取出的解码 PCM，经 MovieAudioSource 适配后送入 WebRTC
    // （详见 MovieAudioSource.h）。音轨在 initialize() 预创建、建连时立即挂上，
    // 以保证其恒定存在于 SDP 中：连接建立后再 AddTrack 会触发重新协商，
    // 而 PeerLink 只处理初始协商。非共享时用 setMovieAudioEnabled(false) 关闭。
    bool movieAudioReady() const {
        return m_localMovieAudioTrack != nullptr;
    }

    // 推送一帧电影 PCM（交错 int16）。data 长度 = samplesPerChannel * channels。
    void pushMovieAudioPcm(const int16_t *data, size_t samplesPerChannel,
                           int sampleRate, size_t channels);

    // 开关电影音轨。暂停或非共享时关闭以节省带宽，轨道本身保留在 SDP 中不动。
    void setMovieAudioEnabled(bool enabled);

    bool movieAudioEnabled() const;

    // ---- 接收端音量（0.0 ~ 1.0，1.0 为原始音量）----
    // 两条路径的音量由不同机制实现：
    //   语音 → WebRTC 自行播放，音量经 AudioSource::SetVolume
    //   电影 → 由 MovieAudioPlayer 播放，音量经 QAudioSink::setVolume
    void setRemoteMovieVolume(double volume);
    void setRemoteChatVolume(double volume);

    double remoteMovieVolume() const {
        return m_remoteMovieVolume;
    }

    double remoteChatVolume() const {
        return m_remoteChatVolume;
    }

    // 开关麦克风。
    void setAudioEnabled(bool enabled);

    bool audioEnabled() const {
        return m_localAudioTrack != nullptr && m_localAudioTrack->enabled();
    }

    // ---- 音频设备（麦克风 / 听筒）----
    // 只影响 voice 连接（真声卡）。media 连接是假声卡、不涉及设备，但观众侧电影声
    // 由 MovieAudioPlayer 播放，听筒需一并切换，否则语音在耳机而电影声在外放。
    // 以下事实均核对过 libwebrtc 源码（含源码位置与行为细节，见 .cpp 中同名小节）：
    // ADM 惰性初始化（未进房时枚举返回 -1，需自行补 Init）；「系统默认」不是设备表
    // 里的某一项，各平台表达方式不同（见 kDefaultDevice）；Linux 无 GUID 可用，
    // 只能按名字识别；索引插拔即漂移，不可持久化；播放/录音已启动时 SetXxxDevice
    // 返回 -1，须走 Stop → Set → Init → Start；引擎重新初始化会复位设备选择；
    // 这些调用必须在 voice worker 线程执行。

    // 「跟随系统默认」在内部的表示。它**不是**一个 ADM 索引：ADM 的设备表里没有
    // "默认"这一项，各平台表达默认的方式不同，统一在 .cpp 的 setDefaultDevice() 里分叉。
    static constexpr int kDefaultDevice = -1;

    // 一个声卡设备。index 为 ADM 索引，name 为显示名
    // （Linux 上无 GUID 可用，故名字是唯一标识）
    struct DeviceInfo {
        int index = 0;
        QString name;

        bool operator==(const DeviceInfo &other) const {
            return index == other.index && name == other.name;
        }
    };

    // 设备列表，QML 的 {label, value} 形状（与 CookieManager::browsers 一致）。
    // 首项恒为「跟随系统默认」，value 为 ""；其余为 ADM 实际枚举出的设备。
    QVariantList recordingDevices() const;
    QVariantList playoutDevices() const;

    // 当前选中的设备名。"" = 系统默认。
    QString recordingDeviceName() const;
    QString playoutDeviceName() const;

    // 重新枚举设备。必要时会先替 WebRTC 完成声卡 Init（设置页在未进房间时
    // 也需看到列表）。仅当列表发生变化时才发出 audioDevicesChanged。
    void refreshAudioDevices();

    // 按名字选择设备，空串 = 系统默认。名字不匹配（设备已拔出）时退回系统默认。
    // 听筒的切换会同时应用到语音（ADM）与电影声（MovieAudioPlayer）。
    // 返回是否真正落地——声卡尚未就绪时仅记下意图（进房后再套用）。
    bool selectRecordingDevice(const QString &name);
    bool selectPlayoutDevice(const QString &name);

    // 销毁全部资源，包括线程与工厂。析构时自动调用。
    void destroy();

signals:
    // 麦克风的实际可用状态变化。QML 用它驱动按钮显示。
    void audioEnabledChanged(bool enabled);

    // 电影音轨的开关状态。同 audioEnabledChanged，由 QML 驱动显示。
    void movieAudioEnabledChanged(bool enabled);

    // 推送画质档位（共享模式下房主修改档位）
    void videoQualityChanged(VideoQuality quality);

    // 接收端两条音轨的音量。QML 的两个滑块绑定此信号。
    void remoteMovieVolumeChanged(double volume);
    void remoteChatVolumeChanged(double volume);

    // 设备列表变化（重新枚举后内容不同）。QML 的设备列表绑定此信号。
    void audioDevicesChanged();

    // 选中的设备变化。名字为 "" 表示系统默认。QML 显示当前选择与落盘均使用它。
    void recordingDeviceChanged(const QString &name);
    void playoutDeviceChanged(const QString &name);

private:
    // 一个对端的两条连接。裸指针 + Qt 父子关系：
    // PeerLink 的 parent 为本对象，管理器析构时子对象自动善后。
    // 但 destroy() 中仍需显式先关连接再停线程，顺序见 .cpp。
    struct PeerLinks {
        PeerLink *voice = nullptr;
        PeerLink *media = nullptr;
    };

    bool initializeVoiceFactory();
    bool initializeMediaFactory();

    // 惰性创建音源与音轨，全进程只创建一次。失败返回 nullptr。
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> getOrCreateAudioTrack();
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> getOrCreateVideoTrack();
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> getOrCreateMovieAudioTrack();

    // 创建一条连接并挂上应挂的轨道。失败返回 nullptr。
    PeerLink *createLink(const QString &peerId, LinkKind kind,
                         bool createDataChannel);

    // 将 m_iceServers 转换为 libwebrtc 的形状。全项目唯一的转换点——
    // libwebrtc 侧字段名为 password、协议中为 credential，对应关系只在此处定义。
    // 列表为空时退回一个公共 STUN（见 .cpp 中的 defaultIceServers）。
    std::vector<webrtc::PeerConnectionInterface::IceServer> buildIceServers() const;

    // ---- 音频设备（内部）----

    // 将 fn 投递到 voice 的 worker 线程并同步执行完毕后返回。
    // 原因见 .cpp 中的实现说明。
    void runOnVoiceWorker(const std::function<void()> &fn);

    // 以下四个必须在 worker 线程上调用（由 runOnVoiceWorker 包裹执行）：

    // 声卡未初始化时先替其执行一次 Init（幂等）。缺少此步骤，枚举只会得到 -1。
    bool ensureVoiceAdmOnWorker();
    std::vector<DeviceInfo> enumerateDevicesOnWorker(bool recording);
    // 套用设备。index 为 ADM 索引，kDefaultDevice 表示跟随系统默认。
    // 内部按「先停、再设、再起」执行——热改会失败。
    bool applyRecordingDeviceOnWorker(int index);
    bool applyPlayoutDeviceOnWorker(int index);

    // 以下几个在 Qt 主线程上执行：

    // 重新枚举并写回成员。列表变化时发出 audioDevicesChanged。
    void reloadDevices();
    // 将某个名字落到 ADM 上。名字为空或不匹配时均退回系统默认。
    // force=true 时忽略「索引未变」的短路判断（引擎刚初始化，ADM 内部已被复位）。
    bool applyDevice(bool recording, const QString &name, bool force);
    // 记录用户的选择，必要时一并应用到 ADM 与电影声播放器。
    // 返回是否真正落到 ADM 上（声卡未就绪时仅记下意图，返回 false）。
    bool setDesiredDevice(bool recording, const QString &name);

    int currentDeviceIndex(bool recording) const;
    const std::vector<DeviceInfo> &deviceList(bool recording) const;

    std::optional<webrtc::Environment> m_mediaEnvironment;
    // ---- 工厂甲：真声卡，只跑语音 ----
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> m_voiceAdm;
    std::unique_ptr<webrtc::Thread> m_voiceNetworkThread;
    std::unique_ptr<webrtc::Thread> m_voiceWorkerThread;
    std::unique_ptr<webrtc::Thread> m_voiceSignalingThread;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_voiceFactory;

    // ---- 工厂乙：假声卡，只跑视频 + 电影音频 ----
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> m_dummyAdm;
    std::unique_ptr<webrtc::Thread> m_mediaNetworkThread;
    std::unique_ptr<webrtc::Thread> m_mediaWorkerThread;
    std::unique_ptr<webrtc::Thread> m_mediaSignalingThread;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_mediaFactory;

    // ---- 声卡的保活锁 ----
    // 一条不做任何事的常驻连接（不挂轨、不协商、不接数据通道），唯一作用是使
    // WebRTC 的 media engine 引用计数永不为 0；本质是 libwebrtc 上游缺陷的补丁，
    // 详见 initializeVoiceFactory()，修复后可整块删除。
    // 声明顺序重要：观察者必须先于连接声明（成员逆序析构），否则连接销毁时的
    // 回调可能访问到已析构的观察者。
    class KeepAliveObserver : public webrtc::PeerConnectionObserver {
    public:
        void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
        void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
        void OnRenegotiationNeeded() override {}
        void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) override {}
        void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
        void OnIceCandidate(const webrtc::IceCandidateInterface *) override {}
        void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>) override {}
    };

    KeepAliveObserver m_voiceKeepAliveObserver;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> m_voiceKeepAlive;

    // 当前使用的 ICE 配置（STUN + 可能带临时凭据的 TURN）。
    // 由 SignalingChannel 在每次建连前写入，createLink 读取。
    QList<IceServerConfig> m_iceServers;

    QHash<QString, PeerLinks> m_peers;

    // ---- 音频设备 ----
    // 枚举结果。只由 Qt 主线程读写（worker 线程上的枚举结果先拷贝再写回）。
    std::vector<DeviceInfo> m_recordingDevices;
    std::vector<DeviceInfo> m_playoutDevices;

    // ADM 当前实际使用的索引。ADM 没有查询接口，只能自行记录——
    // 引擎初始化时会按平台复位（Windows 是默认通信设备，其余是索引 0），
    // 故每次重新套用后都需同步更新。初值与「未选过设备」的情况一致。
    int m_recordingDeviceIndex = kDefaultDevice;
    int m_playoutDeviceIndex = kDefaultDevice;

    // 用户的选择（即意图）。"" 表示系统默认。跨重启由 AppSettings 保管，
    // 此处只负责在引擎（重新）初始化后将其套回。
    QString m_desiredRecordingName;
    QString m_desiredPlayoutName;

    // 音频源，描述「从哪取音」（麦克风）。全进程一个：所有对端共用同一个麦克风，
    // 只是各自有一条独立的 RTP 流。
    webrtc::scoped_refptr<webrtc::AudioSourceInterface> m_audioSource;

    // 音轨是「把音频源接到某个 PeerConnection 上」的插头。
    // 此处只保存一份原型，每个对端用它 AddTrack，WebRTC 内部会各自建立一条流。
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> m_localAudioTrack;

    // 视频源。所有对端共用同一个帧来源，各自有一条独立的 RTP 流。
    webrtc::scoped_refptr<VideoTrackSource> m_videoSource;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_localVideoTrack;

    // 电影音频侧。音源由本类自行实现（非声卡采集）。
    webrtc::scoped_refptr<MovieAudioSource> m_movieAudioSource;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> m_localMovieAudioTrack;

    // 接收端电影声的播放器。假声卡无法放音，故这一段自行接管：
    // WebRTC 解码出的 PCM 交由 QAudioSink 播放，音量也归它管理。
    std::unique_ptr<MovieAudioPlayer> m_movieAudioPlayer;

    // 接收端音量。只由 Qt 主线程读写（音量滑块 + Q_PROPERTY），无竞争。
    double m_remoteMovieVolume = 1.0;
    double m_remoteChatVolume = 1.0;

    // 当前推送画质。默认为原画（不额外设限）
    VideoQuality m_videoQuality = VideoQuality::Original;

    // 接收侧：WebRTC 帧 → QVideoFrame → QVideoSink。
    // 所有对端共用一个——房间里同时只显示一路视频。
    std::unique_ptr<RemoteVideoRenderer> m_remoteRenderer;

    bool m_initialized = false;
};

#endif //SYNCINE_WEBRTCMANAGER_H
