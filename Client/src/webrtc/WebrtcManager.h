//
// Created by donggu on 2026/9/10.
//

#ifndef SYNCINE_WEBRTCMANAGER_H
#define SYNCINE_WEBRTCMANAGER_H

#include <memory>
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

class WebrtcManager : public QObject,
                      public webrtc::PeerConnectionObserver,
                      public webrtc::CreateSessionDescriptionObserver,
                      public webrtc::SetSessionDescriptionObserver{
    Q_OBJECT

public:
    WebrtcManager();
    ~WebrtcManager() override;

    WebrtcManager(const WebrtcManager&) = delete;
    WebrtcManager& operator=(const WebrtcManager&) = delete;

public:
    // 生命周期由外部管理(QObject 栈对象),不参与 WebRTC 的引用计数;
    // Release 返回 kOtherRefsRemained,告诉 WebRTC 不要 delete 我们
    void AddRef() const override {
    }

    webrtc::RefCountReleaseStatus Release() const override {
        return webrtc::RefCountReleaseStatus::kOtherRefsRemained;
    }

    // 初始化webrtc
    bool initialize(const std::vector<std::string> &stunServers);
    // 创建PeerConnection
    bool createPeerConnection();
    // 创建Offer
    bool createOffer();
    // 创建Answer
    bool createAnswer();
    // 设置远端SDP
    bool setRemoteDescription(const std::string &sdp, const std::string &type);
    // 添加远端ICE Candidate(直接接收信令里的字符串,内部解析)
    bool addIceCandidate(const std::string &sdp, const std::string &sdpMid, int sdpMLineIndex);
    // 销毁
    void destroy();

    bool isInitialized() const {
        return m_initialized;
    }

    bool hasPeerConnection() const {
        return m_peerConnection != nullptr;
    }

protected:
    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override;
    void OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override;
    void OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override;
    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override;
    void OnRenegotiationNeeded() override;
    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) override;
    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override;
    void OnIceCandidate(const webrtc::IceCandidate *candidate) override;
    void OnIceConnectionReceivingChange(bool) override;
    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;
    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState) override;
    void OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override;
    void OnIceCandidateError(const std::string &, int, const std::string &, int, const std::string &) override;

    void OnSuccess(webrtc::SessionDescriptionInterface* desc) override;
    void OnFailure(webrtc::RTCError error) override;

    void OnSuccess() override;

private:
    bool initializeThreads();
    bool initializeFactory();

signals:
    // 注意:这些信号在 WebRTC 信令线程里发出,Qt 会以 queued 方式投递到 GUI 线程,
    // 所以参数必须用 Qt 已注册的类型(QString/int),不能用 std::string
    void offerCreated(const QString &sdp);
    void answerCreated(const QString &sdp);
    void iceCandidateCreated(const QString &sdp, const QString &sdpMid, int sdpMLineIndex);
    void errorOccurred(const QString &message);

private:
    enum class PendingOperation {
        None,
        SetLocalOffer,
        SetLocalAnswer,
        SetRemoteOffer,
        SetRemoteAnswer
    };

    std::unique_ptr<webrtc::Thread> m_networkThread;
    std::unique_ptr<webrtc::Thread> m_workerThread;
    std::unique_ptr<webrtc::Thread> m_signalingThread;

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_peerConnectionFactory;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> m_peerConnection;
    std::vector<std::string> m_stunServers;
    bool m_initialized = false;
    PendingOperation m_pendingOperation = PendingOperation::None;
};


#endif //SYNCINE_WEBRTCMANAGER_H
