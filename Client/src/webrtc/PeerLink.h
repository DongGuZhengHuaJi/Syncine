//
// 一个对端的 WebRTC 连接。
//

#ifndef SYNCINE_PEERLINK_H
#define SYNCINE_PEERLINK_H

#include <atomic>

#include <QObject>
#include <QString>
#include <QVector>

#include "api/peer_connection_interface.h"
#include "api/media_stream_interface.h"
#include "api/video/video_sink_interface.h"

class WebrtcManager;

class PeerLink : public QObject,
                 public webrtc::PeerConnectionObserver,
                 public webrtc::CreateSessionDescriptionObserver,
                 public webrtc::SetSessionDescriptionObserver {
    Q_OBJECT

public:
    PeerLink(const QString &peerId,
             webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory,
             const webrtc::PeerConnectionInterface::RTCConfiguration &config,
             bool createDataChannel,
             QObject *parent = nullptr);

    void close();

    QString peerId() const {
        return m_peerId;
    }

    bool isValid() const {
        return m_connection != nullptr;
    }

    // ---- 信令入站(由 WebrtcManager 转发)----
    void createOffer();
    void setRemoteDescription(const QString &sdp, const QString &type);
    void addIceCandidate(const QString &sdp, const QString &sdpMid, int sdpMLineIndex);

    // 添加本地音轨
    void addLocalAudioTrack(webrtc::scoped_refptr<webrtc::AudioTrackInterface> track);
    // 添加本地视频轨
    void addLocalVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track);

    // 设置远端视频轨的渲染器。由 WebrtcManager 注入,生命周期比本对象长。
    void setRemoteVideoSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink);

    // 临时闭麦
    void setAudioMuted(bool muted);

    // 生命周期由 WebrtcManager 管理,Release返回 kOtherRefsRemained
    void AddRef() const override {
        ++m_refCount;
    }

    webrtc::RefCountReleaseStatus Release() const override {
        --m_refCount;
        return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
    }

signals:
    void offerCreated(const QString &peerId, const QString &sdp);
    void answerCreated(const QString &peerId, const QString &sdp);
    void iceCandidateCreated(const QString &peerId, const QString &sdp,
                             const QString &sdpMid, int sdpMLineIndex);
    void connected(const QString &peerId);
    void closed(const QString &peerId);
    void errorOccurred(const QString &peerId, const QString &message);

private:
    // ---- PeerConnectionObserver ----
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override;
    void OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override;
    void OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) override;
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override;
    void OnRenegotiationNeeded() override;
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) override;
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override;
    void OnIceCandidate(const webrtc::IceCandidate *) override;
    void OnIceConnectionReceivingChange(bool) override;
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface>) override;
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState) override;
    void OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) override;
    void OnIceCandidateError(const std::string &address, int port, const std::string &url,
                             int error_code, const std::string &error_text) override;

    // ---- CreateSessionDescriptionObserver / SetSessionDescriptionObserver ----
    void OnSuccess(webrtc::SessionDescriptionInterface *desc) override;
    void OnFailure(webrtc::RTCError error) override;
    void OnSuccess() override;

private:
    enum class PendingOperation {
        None,
        SetLocalOffer,
        SetLocalAnswer,
        SetRemoteOffer,
        SetRemoteAnswer,
    };

    // 远端描述设好之前到达的 ICE 先存着,否则 AddIceCandidate 会失败
    void flushPendingCandidates();
    // 应答方专用:收到远端 offer 后生成本地 answer
    void createAnswer();
    void emitError(const QString &message);

    QString m_peerId;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_factory;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> m_connection;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> m_localAudioTrack;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_localVideoTrack;

    // 对端的视频轨
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_remoteVideoTrack;

    // 远端画面往哪送。由 WebrtcManager 注入,生命周期比本对象长。
    webrtc::VideoSinkInterface<webrtc::VideoFrame> *m_remoteVideoSink = nullptr;

    // 等待执行的操作
    PendingOperation m_pendingOperation = PendingOperation::None;

    // 已设好远端描述?
    bool m_remoteDescriptionSet = false;
    QVector<QString> m_pendingCandidateSdps;
    QVector<QString> m_pendingCandidateMids;
    QVector<int> m_pendingCandidateIndexes;

    // 可变的,因为 AddRef/Release 是 const 方法
    mutable std::atomic<int> m_refCount{0};
};

#endif //SYNCINE_PEERLINK_H
