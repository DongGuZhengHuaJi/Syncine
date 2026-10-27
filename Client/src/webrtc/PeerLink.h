//
// 一个对端的 WebRTC 连接。
//

#ifndef SYNCINE_PEERLINK_H
#define SYNCINE_PEERLINK_H

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include "api/data_channel_interface.h"
#include "api/peer_connection_interface.h"
#include "api/media_stream_interface.h"
#include "api/rtp_sender_interface.h"
#include "api/video/video_sink_interface.h"
#include "rtc_base/synchronization/mutex.h"

class WebrtcManager;


// 有两种连接：Voice(语音)和 Media(电影/视频)。每条连接只承载它该承载的轨，由 LinkKind 决定：
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
    ~PeerLink() override;

    void close();

    QString peerId() const {
        return m_peerId;
    }

    // 这条连接是哪一路。所有出站信令都带上它，好让对端知道该喂给哪条连接。
    LinkKind kind() const {
        return m_kind;
    }

    QString linkId() const {
        return linkKindId(m_kind);
    }

    bool isValid() const {
        return m_connection != nullptr;
    }

    // ---- 信令入站（由 WebrtcManager 转发）----
    // iceRestart=true 时，生成的 offer 会换一组 ICE ufrag/pwd 并重新收集候选：
    // 轨道、DataChannel、DTLS 都保留，只换网络路径。前提是当前没有别的协商
    // 在进行（signaling state 必须是 stable）。
    void createOffer(bool iceRestart = false);
    // 请求一次 ICE restart。协商正忙时返回 false，调用方稍后重试。
    bool restartIce();
    void setRemoteDescription(const QString &sdp, const QString &type);
    void addIceCandidate(const QString &sdp, const QString &sdpMid, int sdpMLineIndex);

    // ---- 本地轨挂载 ----
    // 每条连接只承载它该承载的轨，由 LinkKind 决定，调用错了会被挡下：
    //   Voice → addLocalAudioTrack(麦克风)
    //   Media → addLocalVideoTrack + addLocalMovieAudioTrack
    static constexpr const char *kMovieStreamId = "syncine-movie";
    static constexpr const char *kChatStreamId = "syncine-mic";

    // 添加本地音轨（只允许 Voice 连接调用）
    void addLocalAudioTrack(webrtc::scoped_refptr<webrtc::AudioTrackInterface> track);
    // 添加本地视频轨（只允许 Media 连接调用）
    void addLocalVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track);
    // 添加本地电影音轨（只允许 Media 连接调用）。
    // sending 决定初始 RTP 参数里的 active：false 时发送流从协商起就不启动。
    void addLocalMovieAudioTrack(webrtc::scoped_refptr<webrtc::AudioTrackInterface> track,
                                 bool sending);

    // 控制是否真正发送电影音轨（RTP 参数 encodings[0].active）。
    // false = 停发 RTP，不只是把它静音；SDP 不变，恢复时无需重新协商。
    void applyMovieAudioSending(bool active);

    // 设置远端视频轨的渲染器。由 WebrtcManager 注入，生命周期比本对象长。
    void setRemoteVideoSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink);

    // ---- 推送画质（共享模式下由房主设定）----
    //
    // maxBitrateBps / maxFramerate 传 0 表示这一项不设限。
    // 注意这只是**上限**:实际码率仍由拥塞控制在这个上限内跑，
    // "不设限"也不等于无限 —— 那样拿到的是 WebRTC 的默认上限（540p 以上约 2.5Mbps）。
    // 可随时调用（界面线程）,内部会投递到信令线程。
    void applyVideoQuality(int maxBitrateBps, double maxFramerate);

    // 显式调用 setPlayoutEnabled(false)，让 NullAudioPoller 每隔约 10 毫秒请求一次音频数据
    void setPlayoutEnabled(bool enabled);

    // 远端音轨到了往哪送
    // Voice 连接无需设置，由 WebRTC 自己播；Movie 连接要设置，交给 MovieAudioPlayer 播。
    void setRemoteAudioSink(webrtc::AudioTrackSinkInterface *sink);

    // 设置远端音轨的音量。0.0 ~ 1.0,1.0 为原始音量。
    // 可从任意线程调用 —— 内部会投递到 WebRTC 信令线程再执行，因为AudioRtpReceiver::OnSetVolume 要求运行在信令线程
    void setRemoteAudioVolume(double volume);

    // 临时闭麦
    void setAudioMuted(bool muted);

    // ---- 数据通道（文件传输）----
    // 两条通道，只在 offer 方创建，answer 方通过 OnDataChannel 拿到：
    //   "ctl"  -> syncine-file-ctl，reliable + unordered，走控制 JSON
    //   "data" -> syncine-file-data，reliable + ordered，走文件块
    // 控制与数据分开后，控制消息不会被大量文件块的重传队列堵住。
    bool isDataChannelOpen(const QString &channel) const;
    // 指定通道里还没发出去的字节数（SCTP 发送缓冲）。文件传输据此节流。
    quint64 dataChannelBufferedAmount(const QString &channel) const;
    // 发送一条消息。通道未打开或队列已满时返回 false
    bool sendDataChannelMessage(const QString &channel, const QByteArray &data,
                                bool binary);

    // 采集一次 WebRTC GetStats，输出一条诊断日志：
    // 选中 candidate 类型、RTT、可用带宽、DataChannel 收发字节。
    // 由 FileTransferManager 在传输期间低频调用（默认 15 秒一次）。
    void requestDataChannelStats();

    // 生命周期由 WebrtcManager 管理，Release返回 kOtherRefsRemained
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
    // ICE 断开（可能自行恢复）与 ICE 失败（不会自行恢复）。
    // 这两个回调运行在 WebRTC 信令线程，重启决策由 SignalingChannel 在
    // 主线程做，不要在这里直接发起协商。
    void iceDisconnected(const QString &peerId, const QString &link);
    void iceFailed(const QString &peerId, const QString &link);

    // 数据通道状态与消息。channel 为 "ctl" 或 "data"；信号可能从 WebRTC
    // 信令线程发出，接收方位于主线程时由 Qt 自动排队投递
    void dataChannelOpened(const QString &peerId, const QString &channel);
    void dataChannelClosed(const QString &peerId, const QString &channel);
    void dataChannelMessageReceived(const QString &peerId, const QString &channel,
                                    bool binary, const QByteArray &data);

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

    // 远端描述设好之前到达的 ICE 先存着，否则 AddIceCandidate 会失败
    void flushPendingCandidates();
    // 应答方专用：收到远端 offer 后生成本地 answer
    void createAnswer();
    void emitError(const QString &message);
    // 把当前画质档位写进视频 sender 的参数（内部会切到信令线程）
    void pushVideoQuality();
    // 把发送/停发状态写进电影音轨 sender（内部会切到信令线程）
    void pushMovieAudioSending();

    // ---- 数据通道 ----
    // 观察者由 DataChannelInterface 在信令线程回调
    class ChannelObserver;
    enum class DataChannelKind {
        Control,
        Data,
    };
    struct ChannelSlot {
        webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
        bool openReported = false;
        bool closedReported = false;
    };

    ChannelSlot &slotFor(DataChannelKind kind);
    const ChannelSlot &slotFor(DataChannelKind kind) const;
    ChannelObserver *observerFor(DataChannelKind kind) const;
    static QString channelId(DataChannelKind kind);
    static std::optional<DataChannelKind> kindFromId(const QString &channel);
    static std::optional<DataChannelKind> kindFromLabel(const std::string &label);
    void attachDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel,
                           DataChannelKind kind);
    void onDataChannelStateChanged(DataChannelKind kind);
    void onDataChannelMessage(DataChannelKind kind, const webrtc::DataBuffer &buffer);

    QString m_peerId;
    LinkKind m_kind;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> m_factory;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> m_connection;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> m_localAudioTrack;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_localVideoTrack;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> m_localMovieAudioTrack;

    // 视频轨对应的 sender。画质档位（码率/帧率上限）只能通过它设置 ——
    // AddTrack 的返回值以前是丢掉的，所以要单独留着。
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> m_videoSender;
    int m_videoMaxBitrateBps = 0;    // 0 = 不设上限(用 WebRTC 默认)
    double m_videoMaxFramerate = 0;  // 0 = 不设上限

    // 电影音轨对应的 sender。encodings[0].active 决定是否真正发送；
    // false 时 AudioSendStream 会 Stop()，不产生任何 RTP 包。
    webrtc::scoped_refptr<webrtc::RtpSenderInterface> m_movieAudioSender;
    // 主线程与信令线程都会读：连接建立后要按它补设一次参数
    bool m_movieAudioSending = true;

    // 对端的视频轨
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> m_remoteVideoTrack;

    // 对端的音频轨（每条连接只有一条）,以及它的音量。
    //
    // 为什么要加锁：轨由**信令线程**在 OnTrack 里写入，而音量滑块在
    // **主线程**读写；m_remoteVolume 则是主线程写、信令线程在 OnTrack 里读。
    // 两个方向都有交叉，所以这组成员统一由 m_remoteAudioLock 保护。
    webrtc::Mutex m_remoteAudioLock;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface>
        m_remoteAudioTrack RTC_GUARDED_BY(m_remoteAudioLock);
    double m_remoteVolume RTC_GUARDED_BY(m_remoteAudioLock) = 1.0;

    // 远端音轨往哪送。不设则交给 WebRTC 自己播。
    webrtc::AudioTrackSinkInterface *m_remoteAudioSink = nullptr;

    // 远端画面往哪送。由 WebrtcManager 注入，生命周期比本对象长。
    webrtc::VideoSinkInterface<webrtc::VideoFrame> *m_remoteVideoSink = nullptr;

    // ---- 数据通道 ----
    // 观察者回调在 WebRTC 信令线程，send/查询在主线程，故两条通道的读写
    // 由 m_dataChannelLock 保护。观察者本身活到本对象析构：base 类的
    // 析构函数是 protected，不能按基类指针删除。
    std::unique_ptr<ChannelObserver> m_controlObserver;
    std::unique_ptr<ChannelObserver> m_dataObserver;
    mutable webrtc::Mutex m_dataChannelLock;
    ChannelSlot m_controlChannel RTC_GUARDED_BY(m_dataChannelLock);
    ChannelSlot m_dataChannel RTC_GUARDED_BY(m_dataChannelLock);
    bool m_dataChannelClosing RTC_GUARDED_BY(m_dataChannelLock) = false;

    // 等待执行的操作。
    // createOffer()/restartIce() 在主线程调用，OnSuccess/OnFailure 在 WebRTC
    // 信令线程回调，所以这个标记必须是原子的。
    std::atomic<PendingOperation> m_pendingOperation{PendingOperation::None};

    // 当前本地 offer 是否带 ICE restart。CreateOffer/SetLocalDescription
    // 失败时据此重新排队，而不是当成普通协商错误放弃。
    std::atomic<bool> m_iceRestartOffer{false};

    // 这条连接是否成功连通过。只有"连通过再断"才值得 ICE restart；
    // 初次就连不上（没有可用候选、端口被防火墙挡住）重启也救不了，
    // 保留原来的立即报错行为。只在 WebRTC 信令线程读写。
    bool m_everConnected = false;

    // 已设好远端描述？
    bool m_remoteDescriptionSet = false;
    QVector<QString> m_pendingCandidateSdps;
    QVector<QString> m_pendingCandidateMids;
    QVector<int> m_pendingCandidateIndexes;

    // 可变的，因为 AddRef/Release 是 const 方法
    mutable std::atomic<int> m_refCount{0};
};

#endif //SYNCINE_PEERLINK_H
