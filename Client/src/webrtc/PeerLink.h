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

    // 唯一正确的销毁方式。
    //
    // 不能直接 delete:此刻对端的回调可能还在 WebRTC 信令线程上跑,虚表里
    // 还有指向本对象的指针。close() 先把连接关掉并释放引用,之后这个对象
    // 就只剩主线程持有,再删才是安全的。
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

    // 本地播放的音轨。留空则本次协商只建数据通道,不涉及媒体。
    //
    // 必须在 createOffer() **之前**调用,否则 SDP 里不会有 m=audio 段。
    void addLocalAudioTrack(webrtc::scoped_refptr<webrtc::AudioTrackInterface> track);

    // 临时闭麦。与 WebrtcManager::setAudioEnabled 的区别:
    //   setAudioEnabled(false) —— 关麦克风,不再采集(省 CPU、指示灯灭)
    //   setAudioMuted(true)    —— 麦克风还开着,但发出去的帧被替换成静音
    //
    // 两者都不会影响连接本身,也都不需要重新协商。
    void setAudioMuted(bool muted);

    // ---- 观察者接口要求的引用计数实现 ----
    // 生命周期由 WebrtcManager 掌控(close() + delete),这里只记账不自杀。
    //
    // 永远返回 kOtherRefsRemained 是有意的:它等于告诉 WebRTC
    // "对象还有人管,你别动"。若返回 kDroppedLastRef,WebRTC 会认为本对象
    // 该自己 delete 自己 —— 而真正的释放权在管理器手上,两边都删就是重复释放。
    // 这个引用计数只用来记录"回调期间有谁在引用我",不承担析构职责。
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

    // 实现细节:下面的观察者回调是 WebRTC 调用的,不该出现在公开接口里。
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

    // 每个对端各一份,这就是多对端串扰的解药
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
