//
// WebRTC 的管理者:线程、编解码工厂、以及所有对端连接。
//
// 职责边界(重构后的样子):
//
//   WebrtcManager  —— 建一次就不变的东西:三根 WebRTC 线程、PeerConnectionFactory。
//                     再加上一张 peerId -> PeerLink 的表,负责创建和销毁对端。
//                     它自己**不参与任何一次 SDP 协商**。
//
//   PeerLink       —— 单个对端的连接和它那台 SDP 协商状态机,见 PeerLink.h。
//
// 之所以这样切:协商状态是**每对端一份**的,放在管理器上就变成了共享变量,
// 两个对端同时协商时会互相踩(详见 PeerLink.h 顶部的说明)。
// 管理器只留全局唯一的东西。
//
// 信令的进出都带 peerId:信号里带出去,入站接口带进来。
// SignalingChannel 因此不需要猜"这条消息是谁的"。
//

#ifndef SYNCINE_WEBRTCMANAGER_H
#define SYNCINE_WEBRTCMANAGER_H

#include <memory>
#include <QHash>
#include <QObject>
#include <QString>
#include <string>
#include <vector>

#include "api/peer_connection_interface.h"
#include "api/create_peerconnection_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/video_codecs/builtin_video_decoder_factory.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/media_stream_interface.h"
#include "rtc_base/thread.h"

#include "PeerLink.h"
#include "RemoteVideoRenderer.h"
#include "VideoTrackSource.h"

class QVideoFrame;
class QVideoSink;

class WebrtcManager : public QObject {
    Q_OBJECT

public:
    WebrtcManager();
    ~WebrtcManager() override;

    WebrtcManager(const WebrtcManager &) = delete;
    WebrtcManager &operator=(const WebrtcManager &) = delete;

    // 初始化三根线程和编解码工厂。可重复调用,第二次直接返回 true。
    bool initialize(const std::vector<std::string> &stunServers);

    // ---- 对端管理 ----

    // 为 peerId 建一条连接。已存在则直接返回 true(幂等)。
    // createDataChannel 默认开:现阶段靠它验证链路是否真的打通,
    // 加音视频轨之后可以关掉。
    bool createPeerConnection(const QString &peerId, bool createDataChannel = true);

    // 关闭并销毁某个对端。对端离开房间时调用。
    void removePeer(const QString &peerId);

    // 关闭所有对端(离开房间时调用)。线程和工厂保留,下次进房继续用。
    void closeAllPeers();

    PeerLink *peer(const QString &peerId) const;

    // 当前所有对端 ID。SignalingChannel 靠它比对成员表,决定该删掉哪些连接。
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
    //
    // 视频源(帧的入口)。链路验证阶段由测试代码直接喂合成帧,
    // 接上真实播放器以后再从 Qt 那边喂。
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

    // 开关麦克风。
    void setAudioEnabled(bool enabled);

    bool audioEnabled() const {
        return m_localAudioTrack != nullptr && m_localAudioTrack->enabled();
    }

    // 本地播放的音轨交给某个对端发送。留空则本次协商只有数据通道。
    void setLocalAudioTrack(webrtc::scoped_refptr<webrtc::AudioTrackInterface> track);

    // 销毁一切,包括线程和工厂。析构时自动调用。
    void destroy();

signals:
    // 麦克风实际可用状态发生变化。QML 用它驱动按钮的显示。
    void audioEnabledChanged(bool enabled);


private:
    bool initializeThreads();
    bool initializeFactory();

    // 惰性创建音频源和音轨,全进程只建一次。失败返回 nullptr。
    // 放在私有:调用方只该通过 setAudioEnabled 开关麦克风,
    // 不该关心音轨对象本身的生命周期。
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> getOrCreateAudioTrack();

    // 同上,视频侧。必须在任何一次协商之前调用过,
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> getOrCreateVideoTrack();

    std::unique_ptr<webrtc::Thread> m_networkThread;
    std::unique_ptr<webrtc::Thread> m_workerThread;
    std::unique_ptr<webrtc::Thread> m_signalingThread;

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_peerConnectionFactory;
    std::vector<std::string> m_stunServers;

    // 裸指针 + Qt 父子关系:PeerLink 的 parent 是本对象,
    // 管理器析构时子对象自动善后,不需要手工遍历释放。
    // 但 destroy() 里仍要**显式**先关连接再停线程,顺序见 .cpp
    QHash<QString, PeerLink *> m_peers;

    // 音频源描述"从哪取音"(麦克风)。全进程一个:所有对端共用同一个麦克风,
    // 只是各自有一条独立的 RTP 流。
    webrtc::scoped_refptr<webrtc::AudioSourceInterface> m_audioSource;

    // 音轨是"把音频源接到某个 PeerConnection 上"的插头。
    // 这里只保存一份原型,每个对端拿它去 AddTrack,WebRTC 内部会各建一条流。
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> m_localAudioTrack;

    // 视频源。所有对端共用同一个帧来源,各自有一条独立的 RTP 流。
    webrtc::scoped_refptr<VideoTrackSource> m_videoSource;

    // 视频轨原型。和音轨一样,每个对端拿它去 AddTrack。
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_localVideoTrack;

    // 接收侧:WebRTC 帧 → QVideoFrame → QVideoSink。
    // 所有对端共用一个 —— 房间里同时只显示一路视频。
    std::unique_ptr<RemoteVideoRenderer> m_remoteRenderer;

    bool m_initialized = false;
};

#endif //SYNCINE_WEBRTCMANAGER_H
