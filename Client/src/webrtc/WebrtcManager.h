//
// WebRTC 的管理者:线程、编解码工厂、以及所有对端连接。
//
// 职责边界:
//
//   WebrtcManager  —— 建一次就不变的东西:WebRTC 线程、PeerConnectionFactory。
//                     再加上一张 peerId -> {voice, media} 的表,负责创建和销毁对端。
//                     它自己**不参与任何一次 SDP 协商**。
//
//   PeerLink       —— 单条连接的 SDP 协商状态机,见 PeerLink.h。
//
// 之所以这样切:协商状态是**每连接一份**的,放在管理器上就变成了共享变量,
// 多个对端同时协商时会互相踩。管理器只留全局唯一的东西。
//
// ---------------------------------------------------------------------------
// 为什么每个对端有**两条** PeerConnection(2026-10 的架构调整)
// ---------------------------------------------------------------------------
//
// 起因是一个绕不过去的 WebRTC 行为:ADM(声卡模块)采集到的声音会被
// **广播给同一个工厂里所有已启动的发送流**(见 audio_state.cc 的
// UpdateAudioTransportWithSendingStreams:它逐个 push,不做任何过滤)。
//
// 于是原先"一条连接上挂语音 + 视频 + 电影音频三条轨"的做法必然出问题:
//   1. 麦克风的声音被灌进电影音轨 —— 观众即使没开麦也听得到房主说话
//   2. 电影音轨被两个线程同时喂(ADM 采集线程 + 我们的 Qt 线程),
//      触发 AudioSendStream::SendAudioData 里的 race 断言 → 进程崩溃
//
// 而 AudioState 是**每个工厂一份**的(见 pc/peer_connection_factory.cc:
// audio_state 来自工厂的 media engine),所以:
//
//   **光加一条 PeerConnection 没用 —— 必须是另一个工厂。**
//
// 于是分成两个完全独立的 WebRTC 世界:
//
//   工厂甲(真声卡)  → voice 连接:只挂麦克风音轨
//   工厂乙(假声卡)  → media 连接:挂视频轨 + 电影音轨
//
// 假声卡(AudioDeviceModule::kDummyAudio)既不采集也不播放,于是:
//   · 广播线上没东西可灌 → 电影音轨干净、不再有 race
//   · 代价:假声卡放不出声音 → 观众端的电影声由 MovieAudioPlayer 自己播
//
// 顺带的好处:轨属于哪条连接一目了然,接收端不用再靠 stream id 去猜身份。
//
// 注意两条连接是**各自独立协商**的:各有各的 SDP、ICE、连接状态,
// 完全可能一条通一条不通。信令里用 link 字段区分(见 Protocol.h)。
//

#ifndef SYNCINE_WEBRTCMANAGER_H
#define SYNCINE_WEBRTCMANAGER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <QHash>
#include <QObject>
#include <QString>
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

    // 初始化两套线程和编解码工厂。可重复调用,第二次直接返回 true。
    bool initialize(const std::vector<std::string> &stunServers);

    // ---- 对端管理 ----

    // 为 peerId 建**两条**连接(voice + media)。已存在则直接返回 true(幂等)。
    // createDataChannel 暂时无作用
    bool createPeerConnection(const QString &peerId, bool createDataChannel = true);

    // 关闭并销毁某个对端的**两条**连接。对端离开房间时调用。
    void removePeer(const QString &peerId);

    // 关闭所有对端(离开房间时调用)。线程和工厂保留,下次进房继续用。
    void closeAllPeers();

    // 取某条连接。kind 决定要哪一条。
    PeerLink *peer(const QString &peerId, LinkKind kind) const;

    // 当前所有对端 ID(不区分连接)。SignalingChannel 靠它比对成员表。
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

    // 视频源(帧的入口)。接上播放器后由 Qt 那边喂。
    VideoTrackSource *videoSource() const {
        return m_videoSource.get();
    }

    // 把一帧送进 WebRTC。没人订阅时是空操作。
    void pushVideoFrame(const webrtc::VideoFrame &frame);

    // 真实入口:喂一帧 Qt 解码出来的画面。
    // 由 PlaybackController 的 videoFrameAvailable 信号驱动(main.cpp 里接线)。
    void pushQtVideoFrame(const QVideoFrame &frame);

    // 远端画面往哪里送。由 main.cpp 装配时设置。
    // 不设的话,收到的远端视频会被丢弃(PeerLink 里会打日志说明)。
    void setRemoteVideoSink(QVideoSink *sink);

    // 渲染器是否往 sink 写帧。
    //
    // 只在"显示远端"时开启:显示本地时渲染器收到的都是回声帧,
    // 不该渲染,否则会盖掉本地画面、并维持回声环(见 PlaybackController::applyVideoSink)。
    void setRemoteVideoEnabled(bool enabled);

    // ---- 电影音频 ----
    //
    // 数据不是来自声卡,而是 PlaybackController 用 QAudioBufferOutput 从
    // QMediaPlayer 取出的解码 PCM,经 MovieAudioSource 适配后送进 WebRTC。
    // 见 MovieAudioSource.h 顶部。
    //
    // 音轨在 initialize() 预创建、在 createPeerConnection() 里立刻挂上,
    // 保证它**恒定存在于 SDP 中**:连接建好后再 AddTrack 会触发重新协商,
    // 而 PeerLink 目前只处理初始协商,没有重新协商的状态机。
    // 不共享时用 setMovieAudioEnabled(false) 关掉即可,不需要动 SDP。

    bool movieAudioReady() const {
        return m_localMovieAudioTrack != nullptr;
    }

    // 推一帧电影 PCM(交错 int16)。data 长度 = samplesPerChannel * channels。
    void pushMovieAudioPcm(const int16_t *data, size_t samplesPerChannel,
                           int sampleRate, size_t channels);

    // 开关电影音轨。暂停/非共享时关掉省带宽,轨本身留在 SDP 里不动。
    void setMovieAudioEnabled(bool enabled);

    bool movieAudioEnabled() const;

    // ---- 接收端音量(0.0 ~ 1.0,1.0 为原始音量)----
    //
    // 两条路的音量**由不同机制实现**,这一点值得记住:
    //   语音 → WebRTC 自己的播放,音量走 AudioSource::SetVolume
    //   电影 → 我们的 MovieAudioPlayer 在播,音量走 QAudioSink::setVolume
    // 两者互不相干,所以能独立调 —— 这正是需求要的。
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

    // 销毁一切,包括线程和工厂。析构时自动调用。
    void destroy();

signals:
    // 麦克风实际可用状态发生变化。QML 用它驱动按钮的显示。
    void audioEnabledChanged(bool enabled);

    // 电影音轨的开关状态。同 audioEnabledChanged,由 QML 驱动显示。
    void movieAudioEnabledChanged(bool enabled);

    // 接收端两条音轨的音量。QML 的两个滑块绑这个。
    void remoteMovieVolumeChanged(double volume);
    void remoteChatVolumeChanged(double volume);

private:
    // 一个对端的两条连接。裸指针 + Qt 父子关系:
    // PeerLink 的 parent 是本对象,管理器析构时子对象自动善后。
    // 但 destroy() 里仍要**显式**先关连接再停线程,顺序见 .cpp。
    struct PeerLinks {
        PeerLink *voice = nullptr;
        PeerLink *media = nullptr;
    };

    bool initializeVoiceFactory();
    bool initializeMediaFactory();

    // 惰性创建音源和音轨,全进程只建一次。失败返回 nullptr。
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> getOrCreateAudioTrack();
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> getOrCreateVideoTrack();
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> getOrCreateMovieAudioTrack();

    // 建一条连接并挂上该挂的轨。失败返回 nullptr。
    PeerLink *createLink(const QString &peerId, LinkKind kind,
                         bool createDataChannel);

    // ---- 工厂甲:真声卡,只跑语音 ----
    std::unique_ptr<webrtc::Thread> m_voiceNetworkThread;
    std::unique_ptr<webrtc::Thread> m_voiceWorkerThread;
    std::unique_ptr<webrtc::Thread> m_voiceSignalingThread;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_voiceFactory;

    // ---- 工厂乙:假声卡,只跑视频 + 电影音频 ----
    //
    // 用 optional 而不是直接放一个 Environment:它的默认构造函数被
    // = delete 了,只能由 EnvironmentFactory/CreateEnvironment 造出来。
    //
    // 声明在 m_dummyAdm **之前**,保证它比假声卡活得久
    // (成员按声明顺序构造、逆序析构)。
    std::optional<webrtc::Environment> m_mediaEnvironment;
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> m_dummyAdm;
    std::unique_ptr<webrtc::Thread> m_mediaNetworkThread;
    std::unique_ptr<webrtc::Thread> m_mediaWorkerThread;
    std::unique_ptr<webrtc::Thread> m_mediaSignalingThread;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_mediaFactory;

    std::vector<std::string> m_stunServers;

    QHash<QString, PeerLinks> m_peers;

    // 音频源描述"从哪取音"(麦克风)。全进程一个:所有对端共用同一个麦克风,
    // 只是各自有一条独立的 RTP 流。
    webrtc::scoped_refptr<webrtc::AudioSourceInterface> m_audioSource;

    // 音轨是"把音频源接到某个 PeerConnection 上"的插头。
    // 这里只保存一份原型,每个对端拿它去 AddTrack,WebRTC 内部会各建一条流。
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> m_localAudioTrack;

    // 视频源。所有对端共用同一个帧来源,各自有一条独立的 RTP 流。
    webrtc::scoped_refptr<VideoTrackSource> m_videoSource;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_localVideoTrack;

    // 电影音频侧。音源是我们自己实现的(不是声卡采集)。
    webrtc::scoped_refptr<MovieAudioSource> m_movieAudioSource;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> m_localMovieAudioTrack;

    // 接收端电影声的播放器。假声卡放不出声音,所以这一段由我们自己接管:
    // WebRTC 解码出的 PCM 交给 QAudioSink 播,音量也归它管。
    std::unique_ptr<MovieAudioPlayer> m_movieAudioPlayer;

    // 接收端音量。只由 Qt 主线程读写(音量滑块 + Q_PROPERTY),不存在竞争。
    double m_remoteMovieVolume = 1.0;
    double m_remoteChatVolume = 1.0;

    // 接收侧:WebRTC 帧 → QVideoFrame → QVideoSink。
    // 所有对端共用一个 —— 房间里同时只显示一路视频。
    std::unique_ptr<RemoteVideoRenderer> m_remoteRenderer;

    bool m_initialized = false;
};

#endif //SYNCINE_WEBRTCMANAGER_H
