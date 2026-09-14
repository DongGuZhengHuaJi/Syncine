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

    // 本地播放的音轨交给某个对端发送。留空则本次协商只有数据通道。
    void setLocalAudioTrack(webrtc::scoped_refptr<webrtc::AudioTrackInterface> track);

    // 销毁一切,包括线程和工厂。析构时自动调用。
    void destroy();

    // 这里**没有** offerCreated / iceCandidateCreated 之类的信号。
    //
    // 那些是 PeerLink 的信号,由 SignalingChannel 在拿到 PeerLink 后直接连。
    // 之前这里做过一层同名转发,但那既没加工参数、也没改语义,只是让
    // "消息从哪来"多绕了一站。去掉之后信号流是一条直路:
    //
    //     PeerLink --(信号)--> SignalingChannel --(网络)--> 对端
    //
    // 本类只负责建、拆、查对端,以及持有线程和工厂。

private:
    bool initializeThreads();
    bool initializeFactory();

    std::unique_ptr<webrtc::Thread> m_networkThread;
    std::unique_ptr<webrtc::Thread> m_workerThread;
    std::unique_ptr<webrtc::Thread> m_signalingThread;

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_peerConnectionFactory;
    std::vector<std::string> m_stunServers;

    // 裸指针 + Qt 父子关系:PeerLink 的 parent 是本对象,
    // 管理器析构时子对象自动善后,不需要手工遍历释放。
    // 但 destroy() 里仍要**显式**先关连接再停线程,顺序见 .cpp
    QHash<QString, PeerLink *> m_peers;

    webrtc::scoped_refptr<webrtc::AudioTrackInterface> m_localAudioTrack;
    bool m_initialized = false;
};

#endif //SYNCINE_WEBRTCMANAGER_H
