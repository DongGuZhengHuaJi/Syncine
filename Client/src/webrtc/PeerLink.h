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
#include "rtc_base/synchronization/mutex.h"

class WebrtcManager;


// 有两种连接:Voice(语音)和 Media(电影/视频)。每条连接只承载它该承载的轨,由 LinkKind 决定:
enum class LinkKind {
    Voice,
    Media,
};

// 用于信令路由和日志的短标识。服务端只是原样转发这个字符串。
inline QString linkKindId(LinkKind kind) {
    return kind == LinkKind::Voice ? QStringLiteral("voice") : QStringLiteral("media");
}

class PeerLink : public QObject,
                 public webrtc::PeerConnectionObserver,
                 public webrtc::CreateSessionDescriptionObserver,
                 public webrtc::SetSessionDescriptionObserver {
    Q_OBJECT

public:
    PeerLink(const QString &peerId,
             LinkKind kind,
             webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory,
             const webrtc::PeerConnectionInterface::RTCConfiguration &config,
             bool createDataChannel,
             QObject *parent = nullptr);

    void close();

    QString peerId() const {
        return m_peerId;
    }

    // 这条连接是哪一路。所有出站信令都带上它,好让对端知道该喂给哪条连接。
    LinkKind kind() const {
        return m_kind;
    }

    QString linkId() const {
        return linkKindId(m_kind);
    }

    bool isValid() const {
        return m_connection != nullptr;
    }

    // ---- 信令入站(由 WebrtcManager 转发)----
    void createOffer();
    void setRemoteDescription(const QString &sdp, const QString &type);
    void addIceCandidate(const QString &sdp, const QString &sdpMid, int sdpMLineIndex);

    // ---- 本地轨挂载 ----
    // 每条连接只承载它该承载的轨,由 LinkKind 决定,调用错了会被挡下:
    //   Voice → addLocalAudioTrack(麦克风)
    //   Media → addLocalVideoTrack + addLocalMovieAudioTrack
    static constexpr const char *kMovieStreamId = "syncine-movie";
    static constexpr const char *kChatStreamId = "syncine-mic";

    // 添加本地音轨(只允许 Voice 连接调用)
    void addLocalAudioTrack(webrtc::scoped_refptr<webrtc::AudioTrackInterface> track);
    // 添加本地视频轨(只允许 Media 连接调用)
    void addLocalVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track);
    // 添加本地电影音轨(只允许 Media 连接调用)
    void addLocalMovieAudioTrack(webrtc::scoped_refptr<webrtc::AudioTrackInterface> track);

    // 设置远端视频轨的渲染器。由 WebrtcManager 注入,生命周期比本对象长。
    void setRemoteVideoSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink);

    // 显式调用 setPlayoutEnabled(false)，让 NullAudioPoller 每隔约 10 毫秒请求一次音频数据
    void setPlayoutEnabled(bool enabled);

    // 远端音轨到了往哪送
    // Voice 连接无需设置，由 WebRTC 自己播；Movie 连接要设置，交给 MovieAudioPlayer 播。
    void setRemoteAudioSink(webrtc::AudioTrackSinkInterface *sink);

    // 设置远端音轨的音量。0.0 ~ 1.0,1.0 为原始音量。
    // 可从任意线程调用 —— 内部会投递到 WebRTC 信令线程再执行,因为AudioRtpReceiver::OnSetVolume 要求运行在信令线程
    void setRemoteAudioVolume(double volume);

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
    void offerCreated(const QString &peerId, const QString &link, const QString &sdp);
    void answerCreated(const QString &peerId, const QString &link, const QString &sdp);
    void iceCandidateCreated(const QString &peerId, const QString &link,
                             const QString &sdp, const QString &sdpMid, int sdpMLineIndex);
    void connected(const QString &peerId, const QString &link);
    void closed(const QString &peerId, const QString &link);
    void errorOccurred(const QString &peerId, const QString &link, const QString &message);

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
    LinkKind m_kind;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_factory;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> m_connection;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> m_localAudioTrack;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_localVideoTrack;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> m_localMovieAudioTrack;

    // 对端的视频轨
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_remoteVideoTrack;

    // 对端的音频轨(每条连接只有一条),以及它的音量。
    //
    // 为什么要加锁:轨由**信令线程**在 OnTrack 里写入,而音量滑块在
    // **主线程**读写;m_remoteVolume 则是主线程写、信令线程在 OnTrack 里读。
    // 两个方向都有交叉,所以这组成员统一由 m_remoteAudioLock 保护。
    webrtc::Mutex m_remoteAudioLock;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface>
        m_remoteAudioTrack RTC_GUARDED_BY(m_remoteAudioLock);
    double m_remoteVolume RTC_GUARDED_BY(m_remoteAudioLock) = 1.0;

    // 远端音轨往哪送。不设则交给 WebRTC 自己播。
    webrtc::AudioTrackSinkInterface *m_remoteAudioSink = nullptr;

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
