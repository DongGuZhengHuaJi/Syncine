//
// Created by donggu on 2026/9/14.
//

#include "PeerLink.h"

#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include "WebrtcManager.h"
#include "core/Log.h"
#include "transfer/TransferChannels.h"

#include "api/make_ref_counted.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/thread.h"

namespace {

// 给一条音轨设音量。
//
// **必须在 WebRTC 信令线程上执行** —— track->GetSource()->SetVolume() 最终会走到
// AudioRtpReceiver::OnSetVolume,那里有 RTC_DCHECK_RUN_ON(&signaling_thread_checker_)。
//
// 单独拆成自由函数是为了让投递到信令线程的 lambda 只捕获 track 本身
// (scoped_refptr 会保住它的命),而**不捕获 this** —— 否则 PeerLink 析构后
// 任务才轮到执行，就会踩到悬空指针。
void setSourceVolume(const webrtc::scoped_refptr<webrtc::AudioTrackInterface> &track,
                     double volume) {
    if (track == nullptr)
        return;

    webrtc::AudioSourceInterface *source = track->GetSource();
    if (source != nullptr)
        source->SetVolume(volume);
}

// 把一条轨上挂着的所有 MediaStream id 拼起来，排障用。
std::string describeStreams(const webrtc::scoped_refptr<webrtc::RtpReceiverInterface> &receiver) {
    if (receiver == nullptr)
        return "<无 receiver>";

    std::string result;
    for (const auto &stream : receiver->streams()) {
        if (stream == nullptr)
            continue;
        if (!result.empty())
            result += ",";
        result += stream->id();
    }
    return result.empty() ? "<无>" : result;
}

QString formatBitrate(double bps) {
    if (bps < 0)
        return QStringLiteral("?");
    if (bps >= 1000000.0)
        return QStringLiteral("%1Mbps").arg(bps / 1000000.0, 0, 'f', 1);
    return QStringLiteral("%1kbps").arg(bps / 1000.0, 0, 'f', 0);
}

QString formatBytes(uint64_t bytes) {
    if (bytes >= 1024ull * 1024ull)
        return QStringLiteral("%1MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    if (bytes >= 1024ull)
        return QStringLiteral("%1KiB").arg(bytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1B").arg(bytes);
}

QString candidateType(const webrtc::RTCIceCandidateStats *candidate) {
    if (candidate == nullptr)
        return QStringLiteral("?");
    return QString::fromStdString(candidate->candidate_type.value_or("?"));
}

// GetStats 回调。只在传输期间被低频调用，不持有 PeerLink，
// 因此即使回调晚于 PeerLink 析构也不会踩到悬空指针。
class StatsLogger : public webrtc::RTCStatsCollectorCallback {
public:
    StatsLogger(QString peerId, QString link)
        : m_peerId(std::move(peerId)),
          m_link(std::move(link)) {
    }

    void OnStatsDelivered(
        const webrtc::scoped_refptr<const webrtc::RTCStatsReport> &report) override {
        if (report == nullptr)
            return;

        const webrtc::RTCIceCandidatePairStats *pair = nullptr;
        for (const auto *candidate :
             report->GetStatsOfType<webrtc::RTCIceCandidatePairStats>()) {
            if (!candidate->nominated.value_or(false))
                continue;
            if (candidate->state.value_or("") == "succeeded") {
                pair = candidate;
                break;
            }
            if (pair == nullptr)
                pair = candidate;
        }

        QString localType = QStringLiteral("?");
        QString remoteType = QStringLiteral("?");
        double rtt = -1;
        double outgoing = -1;
        uint64_t bytesSent = 0;
        uint64_t bytesReceived = 0;
        uint64_t discarded = 0;
        if (pair != nullptr) {
            // candidate 对象有两种具体类型，GetAs<RTCIceCandidateStats> 会因
            // type() 不匹配而取不到，必须用 local/remote 子类。
            const auto *local = report->GetAs<webrtc::RTCLocalIceCandidateStats>(
                pair->local_candidate_id.value_or(""));
            const auto *remote = report->GetAs<webrtc::RTCRemoteIceCandidateStats>(
                pair->remote_candidate_id.value_or(""));
            localType = candidateType(local);
            remoteType = candidateType(remote);
            rtt = pair->current_round_trip_time.value_or(-1);
            outgoing = pair->available_outgoing_bitrate.value_or(-1);
            bytesSent = pair->bytes_sent.value_or(0);
            bytesReceived = pair->bytes_received.value_or(0);
            discarded = pair->packets_discarded_on_send.value_or(0);
        }

        int pairChanges = -1;
        for (const auto *transport :
             report->GetStatsOfType<webrtc::RTCTransportStats>()) {
            if (!transport->selected_candidate_pair_id.has_value())
                continue;
            if (pair != nullptr && transport->selected_candidate_pair_id.value()
                                       != pair->id()) {
                continue;
            }
            pairChanges = static_cast<int>(
                transport->selected_candidate_pair_changes.value_or(0));
            break;
        }

        QString controlBytes = QStringLiteral("-");
        QString dataBytes = QStringLiteral("-");
        for (const auto *channel :
             report->GetStatsOfType<webrtc::RTCDataChannelStats>()) {
            const QString label = QString::fromStdString(channel->label.value_or(""));
            const QString bytes = formatBytes(channel->bytes_sent.value_or(0))
                                  + QStringLiteral("/")
                                  + formatBytes(channel->bytes_received.value_or(0));
            if (label.endsWith(QLatin1String(TransferChannel::ControlLabel)))
                controlBytes = bytes;
            else if (label.endsWith(QLatin1String(TransferChannel::DataLabel)))
                dataBytes = bytes;
        }

        LOG_INFO("WebRTCStats") << m_peerId << m_link
                                << "local=" << localType
                                << "remote=" << remoteType
                                << "rtt="
                                << (rtt >= 0
                                        ? QStringLiteral("%1ms").arg(rtt * 1000.0, 0, 'f', 1)
                                        : QStringLiteral("?"))
                                << "out=" << formatBitrate(outgoing)
                                << "pairSent=" << formatBytes(bytesSent)
                                << "pairRecv=" << formatBytes(bytesReceived)
                                << "discarded=" << discarded
                                << "pairChanges=" << pairChanges
                                << "dc.ctl=" << controlBytes
                                << "dc.data=" << dataBytes;
    }

private:
    QString m_peerId;
    QString m_link;
};

} // namespace

// 数据通道观察者。回调在 WebRTC 信令线程上执行，故只做数据拷贝与转发，
// 所有文件逻辑由主线程的 FileTransferManager 处理。
class PeerLink::ChannelObserver final : public webrtc::DataChannelObserver {
public:
    ChannelObserver(PeerLink *link, DataChannelKind kind)
        : m_link(link),
          m_kind(kind) {
    }

    void OnStateChange() override {
        m_link->onDataChannelStateChanged(m_kind);
    }

    void OnMessage(const webrtc::DataBuffer &buffer) override {
        m_link->onDataChannelMessage(m_kind, buffer);
    }

private:
    PeerLink *m_link = nullptr;
    DataChannelKind m_kind;
};


// ============================
// 构造 / 销毁
// ============================

PeerLink::PeerLink(const QString &peerId,
                   LinkKind kind,
                   webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory,
                   const webrtc::PeerConnectionInterface::RTCConfiguration &config,
                   bool createDataChannel,
                   QObject *parent)
    : QObject(parent),
      m_peerId(peerId),
      m_kind(kind),
      m_factory(std::move(factory)) {

    if (m_factory == nullptr) {
        emitError(QStringLiteral("PeerConnectionFactory 为空"));
        return;
    }

    m_controlObserver = std::make_unique<ChannelObserver>(this, DataChannelKind::Control);
    m_dataObserver = std::make_unique<ChannelObserver>(this, DataChannelKind::Data);

    webrtc::PeerConnectionDependencies dependencies(this);
    auto result = m_factory->CreatePeerConnectionOrError(config, std::move(dependencies));
    if (!result.ok()) {
        emitError(QStringLiteral("创建 PeerConnection 失败: ")
                  + QString::fromStdString(result.error().message()));
        return;
    }
    m_connection = result.value();

    // 创建数据通路，用于文件传输。只有 offer 方创建：answer 方在
    // SetRemoteDescription 后经 OnDataChannel 拿到同样的两条通道。
    // 控制通道无序，避免文件块的重传排队把取消/校验消息堵住；
    // 数据通道保序，接收端按到达顺序写文件。
    if (createDataChannel) {
        const auto createChannel = [this](DataChannelKind kind, const char *label,
                                          bool ordered) {
            webrtc::DataChannelInit init;
            init.ordered = ordered;
            auto channel = m_connection->CreateDataChannelOrError(label, &init);
            if (!channel.ok()) {
                LOG_ERROR("PeerLink") << m_peerId << "创建数据通道失败:"
                                      << label << channel.error().message();
                return;
            }
            attachDataChannel(channel.value(), kind);
        };
        createChannel(DataChannelKind::Control, TransferChannel::ControlLabel,
                      /*ordered=*/false);
        createChannel(DataChannelKind::Data, TransferChannel::DataLabel,
                      /*ordered=*/true);
    }
}

PeerLink::~PeerLink() {
    close();
}

void PeerLink::close() {
    // 先摘观察者再放通道：UnregisterObserver 会阻塞到网络线程完成，
    // 返回后不会再有回调摸到本对象。观察者对象本身留到析构（见头文件）。
    webrtc::scoped_refptr<webrtc::DataChannelInterface> controlChannel;
    webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel;
    {
        webrtc::MutexLock lock(&m_dataChannelLock);
        m_dataChannelClosing = true;
        controlChannel = m_controlChannel.channel;
        dataChannel = m_dataChannel.channel;
        m_controlChannel.channel = nullptr;
        m_dataChannel.channel = nullptr;
    }
    if (controlChannel != nullptr)
        controlChannel->UnregisterObserver();
    if (dataChannel != nullptr)
        dataChannel->UnregisterObserver();

    m_pendingCandidateSdps.clear();
    m_pendingCandidateMids.clear();
    m_pendingCandidateIndexes.clear();
    m_remoteDescriptionSet = false;
    m_pendingOperation = PendingOperation::None;

    // 立刻松开外部播放器/渲染器。
    //
    // 这些指针指向 WebrtcManager 持有的 MovieAudioPlayer / RemoteVideoRenderer。
    // 管理器的 destroy() 里是先关连接、再放它们，而关闭到真正析构之间还有一段
    // deleteLater 的窗口期 —— 只要这期间还有一帧音视频摸进来，就会踩到已经释放
    // 的对象。除了清指针，还必须从轨上 RemoveSink；否则轨仍持有 sink 地址。
    {
        webrtc::MutexLock lock(&m_remoteAudioLock);
        if (m_remoteAudioTrack != nullptr && m_remoteAudioSink != nullptr)
            m_remoteAudioTrack->RemoveSink(m_remoteAudioSink);
        m_remoteAudioTrack = nullptr;
        m_remoteAudioSink = nullptr;
    }
    if (m_remoteVideoTrack != nullptr && m_remoteVideoSink != nullptr)
        m_remoteVideoTrack->RemoveSink(m_remoteVideoSink);
    m_remoteVideoTrack = nullptr;
    m_remoteVideoSink = nullptr;

    if (m_connection != nullptr) {
        // 先断开连接再释放，防止析构过程中发生回调
        m_connection->Close();
        m_connection = nullptr;
    }
    m_factory = nullptr;
}

// ============================
// 信令入站
// ============================

void PeerLink::createOffer(bool iceRestart) {
    if (m_connection == nullptr) {
        emitError(QStringLiteral("连接未建立,无法发起协商"));
        return;
    }
    if (m_pendingOperation != PendingOperation::None) {
        LOG_WARN("PeerLink") << m_peerId << "上一次协商还没结束,忽略重复的 createOffer";
        return;
    }

    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    // ICE restart 只换网络路径（新的 ufrag/pwd + 候选），轨道、DataChannel、
    // DTLS 都不受影响。对端按普通 offer 处理，回 answer 即可。
    options.ice_restart = iceRestart;
    if (iceRestart)
        LOG_INFO("PeerLink") << m_peerId << ":" << linkId() << "发起 ICE restart";

    m_pendingOperation = PendingOperation::SetLocalOffer;
    m_iceRestartOffer.store(iceRestart);
    m_connection->CreateOffer(this, options);
}

bool PeerLink::restartIce() {
    if (m_connection == nullptr)
        return false;

    // 正在做 offer/answer 时不能插队：signaling state 不是 stable，
    // CreateOffer 会失败。调用方（SignalingChannel）隔一会儿再试。
    if (m_pendingOperation != PendingOperation::None) {
        LOG_DEBUG("PeerLink") << m_peerId << ":" << linkId()
                              << "协商进行中,ICE restart 稍后重试";
        return false;
    }

    createOffer(true);
    return true;
}

void PeerLink::setRemoteDescription(const QString &sdp, const QString &type) {
    if (m_connection == nullptr) {
        emitError(QStringLiteral("连接未建立,无法设置远端描述"));
        return;
    }

    webrtc::SdpType sdpType;
    if (type == QLatin1String("offer")) {
        sdpType = webrtc::SdpType::kOffer;
    } else if (type == QLatin1String("answer")) {
        sdpType = webrtc::SdpType::kAnswer;
    } else {
        emitError(QStringLiteral("无法识别的 SDP 类型: ") + type);
        return;
    }

    webrtc::SdpParseError parseError;
    auto description = webrtc::CreateSessionDescription(sdpType,
                                                        sdp.toStdString(),
                                                        &parseError);
    if (description == nullptr) {
        emitError(QStringLiteral("解析 SDP 失败: ")
                  + QString::fromStdString(parseError.description));
        return;
    }

    m_pendingOperation = (sdpType == webrtc::SdpType::kOffer)
                             ? PendingOperation::SetRemoteOffer
                             : PendingOperation::SetRemoteAnswer;

    m_connection->SetRemoteDescription(this, description.release());
}

void PeerLink::addIceCandidate(const QString &sdp, const QString &sdpMid, int sdpMLineIndex) {
    if (m_connection == nullptr)
        return;

    // 远端描述没设好之前，AddIceCandidate 会直接失败。而 ICE 与 SDP 是
    // 两条独立通道，候选完全可能先到（尤其是局域网，候选几乎瞬间产生）。
    // 所以先存队列，等 SetRemoteDescription 成功后再补投。
    if (!m_remoteDescriptionSet) {
        m_pendingCandidateSdps.append(sdp);
        m_pendingCandidateMids.append(sdpMid);
        m_pendingCandidateIndexes.append(sdpMLineIndex);
        return;
    }

    webrtc::SdpParseError parseError;
    // 这个重载返回裸指针（所有权归调用方）,要自己接管
    std::unique_ptr<webrtc::IceCandidate> candidate(
        webrtc::CreateIceCandidate(sdpMid.toStdString(),
                                   sdpMLineIndex,
                                   sdp.toStdString(),
                                   &parseError));
    if (candidate == nullptr) {
        emitError(QStringLiteral("解析 ICE candidate 失败: ")
                  + QString::fromStdString(parseError.description));
        return;
    }

    if (!m_connection->AddIceCandidate(candidate.get())) {
        emitError(QStringLiteral("添加 ICE candidate 失败"));
    }
}

void PeerLink::flushPendingCandidates() {
    if (m_connection == nullptr)
        return;

    const QVector<QString> sdps = m_pendingCandidateSdps;
    const QVector<QString> mids = m_pendingCandidateMids;
    const QVector<int> indexes = m_pendingCandidateIndexes;
    m_pendingCandidateSdps.clear();
    m_pendingCandidateMids.clear();
    m_pendingCandidateIndexes.clear();

    for (int i = 0; i < sdps.size(); ++i) {
        webrtc::SdpParseError parseError;
        std::unique_ptr<webrtc::IceCandidate> candidate(
            webrtc::CreateIceCandidate(mids.at(i).toStdString(),
                                       indexes.at(i),
                                       sdps.at(i).toStdString(),
                                       &parseError));
        if (candidate != nullptr)
            m_connection->AddIceCandidate(candidate.get());
    }
}

void PeerLink::addLocalAudioTrack(webrtc::scoped_refptr<webrtc::AudioTrackInterface> track) {
    if (m_connection == nullptr || track == nullptr)
        return;

    if (m_kind != LinkKind::Voice) {
        LOG_WARN("PeerLink") << (m_peerId + ":" + linkId())
                             << "麦克风音轨只能在 voice 连接上挂载,已忽略";
        return;
    }

    // 已经挂过同一根音轨就直接返回。
    if (m_localAudioTrack != nullptr
        && m_localAudioTrack->id() == track->id()) {
        return;
    }


    // 在createOffer之前挂音轨，SDP中会包含相应的m=audio段
    //
    // 第二个参数是 MediaStream id,它会进 SDP 的 a=msid,接收端靠它认出
    // "这是语音那条"。取值必须和 kChatStreamId 一致。
    auto sender = m_connection->AddTrack(track, {kChatStreamId});
    if (!sender.ok()) {
        LOG_ERROR("PeerLink") << m_peerId << "添加音轨失败:" << sender.error().message();
        return;
    }

    m_localAudioTrack = track;

    LOG_INFO("PeerLink") << m_peerId << "已挂载本地音轨";
}

void PeerLink::addLocalVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track,
                                  bool sending) {
    if (m_connection == nullptr || track == nullptr)
        return;

    if (m_kind != LinkKind::Media) {
        LOG_WARN("PeerLink") << (m_peerId + ":" + linkId())
                             << "视频轨只能在 media 连接挂载,已忽略";
        return;
    }

    // 已经挂过同一根视频轨就直接返回。
    if (m_localVideoTrack != nullptr
        && m_localVideoTrack->id() == track->id()) {
        return;
    }

    // active 作为初始 encoding 参数：非共享模式/观众端从协商起就不发视频。
    // 与电影音轨同理，协商之后还要在连接回调里再写一次。
    m_videoSending = sending;
    webrtc::RtpEncodingParameters videoEncoding;
    videoEncoding.active = sending;

    // 在createOffer之前挂视频轨，SDP中会包含相应的m=video段
    auto sender = m_connection->AddTrack(track, {kMediaStreamId}, {videoEncoding});
    if (!sender.ok()) {
        LOG_ERROR("PeerLink") << m_peerId << "添加视频轨失败:" << sender.error().message();
        return;
    }

    m_localVideoTrack = track;
    // sender 要留着：画质档位（码率/帧率上限）和发送开关都只能通过它设置
    m_videoSender = sender.value();
    pushVideoQuality(); // 新连接先把当前档位套上

    LOG_INFO("PeerLink") << m_peerId << "已挂载本地视频轨"
                         << (sending ? "" : "(初始停发)");
}

void PeerLink::addLocalMovieAudioTrack(
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> track, bool sending) {
    if (m_connection == nullptr || track == nullptr)
        return;

    m_movieAudioSending = sending;

    if (m_kind != LinkKind::Media) {
        LOG_WARN("PeerLink") << (m_peerId + ":" + linkId())
                             << "电影音轨只能在 media 连接挂载,已忽略";
        return;
    }

    if (m_localMovieAudioTrack != nullptr
        && m_localMovieAudioTrack->id() == track->id()) {
        return;
    }

    // stream id 与视频共用 kMediaStreamId：接收侧 libwebrtc 据此把两条流
    // 放进同一个 sync_group，启用音视频投递对齐。
    // active 作为初始 encoding 参数一并传入：非共享模式下发送流从协商时就
    // 不会启动。否则 SetSsrc 会按默认的 active=true 重新应用一次，把建连前
    // 设置的 false 覆盖掉。
    webrtc::RtpEncodingParameters movieEncoding;
    movieEncoding.active = sending;
    auto sender = m_connection->AddTrack(track, {kMediaStreamId}, {movieEncoding});
    if (!sender.ok()) {
        LOG_ERROR("PeerLink") << m_peerId << "添加电影音轨失败:" << sender.error().message();
        return;
    }

    m_localMovieAudioTrack = track;
    // sender 留着：停发/恢复电影音轨要改它的 encodings[0].active
    m_movieAudioSender = sender.value();

    LOG_INFO("PeerLink") << m_peerId << "已挂载本地电影音轨";
}

void PeerLink::setAudioMuted(bool muted) {
    if (m_localAudioTrack == nullptr)
        return;

    m_localAudioTrack->set_enabled(!muted);
}

void PeerLink::setPlayoutEnabled(bool enabled) {
    if (m_connection == nullptr)
        return;

    m_connection->SetAudioPlayout(enabled);
}

void PeerLink::setRemoteAudioSink(webrtc::AudioTrackSinkInterface *sink) {
    m_remoteAudioSink = sink;
}

void PeerLink::setRemoteAudioVolume(double volume) {
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> track;

    // 1. 记下目标音量（轨还没到也要记 —— OnTrack 到达时会套用）,
    //    同时把轨的引用抄一份出来，免得下面投递任务期间被换掉。
    {
        webrtc::MutexLock lock(&m_remoteAudioLock);
        m_remoteVolume = volume;
        track = m_remoteAudioTrack;
    }

    // 连接已经没了：音量已经记下，等下次 OnTrack 自己套用。
    if (m_connection == nullptr || track == nullptr)
        return;

    webrtc::Thread *signalingThread = m_connection->signaling_thread();
    if (signalingThread == nullptr)
        return;

    // 已经在信令线程上就直接设，省一轮投递。
    // (正常不会走到这里 —— 这个接口是给 Qt 主线程的音量滑块调的)
    if (signalingThread->IsCurrent()) {
        setSourceVolume(track, volume);
        return;
    }

    // 2. 投递到信令线程执行。lambda 只捕获 track 和音量值，**不捕获 this**,
    //    这样即使 PeerLink 先析构、任务后执行，也不会踩到悬空指针。
    signalingThread->PostTask(
        [track, volume] { setSourceVolume(track, volume); });
}

// ============================
// 观察者回调
// ============================

void PeerLink::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) {
}

void PeerLink::OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) {
}

void PeerLink::OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface>) {
}

void PeerLink::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
    if (channel == nullptr)
        return;

    const std::optional<DataChannelKind> kind = kindFromLabel(channel->label());
    if (!kind.has_value()) {
        LOG_WARN("PeerLink") << m_peerId << "忽略未知的数据通道:" << channel->label();
        channel->Close();
        return;
    }

    LOG_INFO("PeerLink") << m_peerId << "收到数据通道:" << channel->label();
    attachDataChannel(channel, *kind);

    // 协商完成后再设一次画质：AddTrack 之后立刻设有时还太早
    pushVideoQuality();

    emit connected(m_peerId, linkId());
}

void PeerLink::OnRenegotiationNeeded() {
}

void PeerLink::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) {
    // close() 之后仍可能有排队的回调进来，此时不应再触发恢复流程。
    if (m_connection == nullptr)
        return;

    switch (state) {
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionConnected:
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionCompleted:
        m_everConnected = true;
        // 协商完成后必须再落一次电影音轨的发送状态：协商会重新启动发送流，
        // 只靠建连前的设置会被覆盖。OnConnectionChange 未必在所有版本都触发，
        // 两个回调都补一次是幂等的。
        pushMovieAudioSending();
        pushVideoSending();
        emit connected(m_peerId, linkId());
        break;
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionFailed:
        if (!m_everConnected) {
            // 未连接成功过，不进行重连
            emitError(QStringLiteral("ICE 连接失败"));
            break;
        }
        // Failed 不会自行恢复，必须由应用发起 ICE restart。
        LOG_WARN("PeerLink") << m_peerId << ":" << linkId()
                             << "ICE 连接失败,等待重启";
        emit iceFailed(m_peerId, linkId());
        break;
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionDisconnected:
        if (!m_everConnected)
            break;
        // Disconnected 可能只是短暂抖动，先给一个宽限期；由 SignalingChannel
        // 决定是等它自行恢复还是发起 restart。
        LOG_WARN("PeerLink") << m_peerId << ":" << linkId() << "ICE 断开";
        emit iceDisconnected(m_peerId, linkId());
        break;
    default:
        break;
    }
}

void PeerLink::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) {
}

void PeerLink::OnIceCandidate(const webrtc::IceCandidate *candidate) {
    if (candidate == nullptr)
        return;

    std::string sdp;
    if (!candidate->ToString(&sdp))
        return;

    emit iceCandidateCreated(m_peerId,
                             linkId(),
                             QString::fromStdString(sdp),
                             QString::fromStdString(candidate->sdp_mid()),
                             candidate->sdp_mline_index());
}

void PeerLink::OnIceConnectionReceivingChange(bool) {
}

void PeerLink::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
    if (transceiver == nullptr)
        return;

    auto receiver = transceiver->receiver();
    if (receiver == nullptr)
        return;

    auto track = receiver->track();
    if (track == nullptr)
        return;

    // 把能用来"认出这是哪条轨"的线索都打出来，排障用。
    // 注意 track->id() 是 WebRTC 随机生成的 UUID,认不出是谁；
    // 不过现在也不需要认了 —— 轨是从哪条连接来的，它就是什么。
    LOG_INFO("PeerLink") << (m_peerId + ":" + linkId()) << "收到远端媒体轨:"
                         << track->kind()
                         << "(mid=" << transceiver->mid().value_or("<无>")
                         << ", streams=[" << describeStreams(receiver) << "])";

    // ---- 音频轨 ----
    //
    // 每条连接只有一条音频轨，所以它的身份由 m_kind 直接决定，不用猜：
    //   Voice → 对端的麦克风。放音由 WebRTC 负责（真声卡），本类只控制音量。
    //   Media → 对端的电影声。假声卡无法放音，交由本端自己的 sink 播放。
    if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
        auto audioTrack = webrtc::scoped_refptr<webrtc::AudioTrackInterface>(
            static_cast<webrtc::AudioTrackInterface *>(track.get()));

        double volume = 1.0;
        {
            webrtc::MutexLock lock(&m_remoteAudioLock);
            m_remoteAudioTrack = audioTrack;
            volume = m_remoteVolume;
        }

        if (m_remoteAudioSink != nullptr) {
            // 自行播放的那条（电影）：把轨接到本端的 sink 上，由 MovieAudioPlayer
            // 解码后的 PCM 喂给 QAudioSink。音量也在那边管。
            audioTrack->AddSink(m_remoteAudioSink);
            LOG_INFO("PeerLink") << (m_peerId + ":" + linkId()) << "远端音轨已接到本地播放器";
        } else {
            // 交给 WebRTC 放的那条（语音）。本回调就在信令线程上，
            // 正好满足 SetVolume 的线程要求，可一并套用当前音量。
            setSourceVolume(audioTrack, volume);
            LOG_DEBUG("PeerLink") << (m_peerId + ":" + linkId())
                                  << "远端音轨交给 WebRTC 播放,音量" << volume;
        }
        return;
    }

    // 剩下的按视频处理
    if (track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind)
        return;

    auto videoTrack = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(
        static_cast<webrtc::VideoTrackInterface *>(track.get()));

    // 保留引用避免被释放
    m_remoteVideoTrack = videoTrack;

    // 检查是否有视频渲染器
    if (m_remoteVideoSink == nullptr) {
        LOG_WARN("PeerLink") << m_peerId << "收到视频轨,但没有渲染器可接(推流接收未启用)";
        return;
    }

    // 将远端视频轨接到渲染器上
    m_remoteVideoTrack->AddOrUpdateSink(m_remoteVideoSink, webrtc::VideoSinkWants{});
    LOG_INFO("PeerLink") << m_peerId << "远端视频已接到渲染器";
}

void PeerLink::setRemoteVideoSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink) {
    m_remoteVideoSink = sink;
}

void PeerLink::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) {
    switch (state) {
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
        // 协商完成前设置的 RTP 参数可能没落下去（SendStream 还没建），
        // 连上后补一次。Media 连接的电影音轨就是靠这里真正停发的。
        pushMovieAudioSending();
        pushVideoSending();
        emit connected(m_peerId, linkId());
        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
        emitError(QStringLiteral("连接失败"));
        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
        emit closed(m_peerId, linkId());
        break;
    default:
        break;
    }
}

void PeerLink::OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) {
}

void PeerLink::OnIceCandidateError(const std::string &address, int port, const std::string &url,
                                   int error_code, const std::string &error_text) {
    // 候选收集失败是常态：每个 STUN/TURN URL 会按网卡各试一遍，远距离测试
    // 一次能刷几百行。降到 Debug，默认 Info 级别不输出。
    LOG_DEBUG("PeerLink") << m_peerId << "ICE candidate 出错:" << url << error_code
                          << error_text << "(地址" << address << ":" << port << ")";
}

// ---- CreateOffer / CreateAnswer 返回 ----

void PeerLink::OnSuccess(webrtc::SessionDescriptionInterface *desc) {
    if (desc == nullptr || m_connection == nullptr) {
        emitError(QStringLiteral("协商返回了空描述"));
        return;
    }

    // 诊断：把本地 SDP 里和 Opus 有关的行打出来。音频声道问题排查全靠它 ——
    // fmtp 里有没有 stereo=1,一眼就能看出这条连接的协商是不是立体声
    // (rtpmap 里的 /2 只是能力声明，不算数 —— 见 WebrtcManager.cpp 的
    // StereoOpusAudioEncoderFactory 注释)。
    //
    // 两个子串都要匹配：stereo=1 在 fmtp 行上，而那条行里并没有 "opus" 字样
    // （形如 a=fmtp:111 minptime=10;useinbandfec=1;stereo=1），只按 "opus"
    // 过滤会把最要紧的一行漏掉。
    {
        std::string sdp;
        if (desc->ToString(&sdp)) {
            std::istringstream lines(sdp);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.find("opus") != std::string::npos
                    || line.find("fmtp") != std::string::npos)
                    LOG_TRACE("PeerLink") << m_peerId << "SDP:" << line;
            }
        }
    }

    // 本地描述必须落下去，后续的 SetRemoteDescription 才有基准。
    // OnSuccess() 无参重载会在它成功后被回调。
    m_connection->SetLocalDescription(this, desc);
}

void PeerLink::OnFailure(webrtc::RTCError error) {
    m_pendingOperation = PendingOperation::None;
    const bool wasIceRestart = m_iceRestartOffer.exchange(false);
    LOG_WARN("PeerLink") << m_peerId << ":" << linkId()
                         << "协商失败:" << error.message();
    if (wasIceRestart) {
        // ICE restart 的 offer/answer 自己失败了：重新交给 SignalingChannel
        // 排队重试，而不是当成普通协商错误。
        emit iceFailed(m_peerId, linkId());
        return;
    }
    emitError(QStringLiteral("协商失败: ") + QString::fromStdString(error.message()));
}

// ---- SetLocalDescription / SetRemoteDescription 返回 ----

void PeerLink::OnSuccess() {
    if (m_connection == nullptr)
        return;

    const PendingOperation operation = m_pendingOperation;
    m_pendingOperation = PendingOperation::None;

    switch (operation) {
    case PendingOperation::SetLocalOffer: {
        auto description = m_connection->local_description();
        std::string sdp;
        if (description != nullptr && description->ToString(&sdp))
            emit offerCreated(m_peerId, linkId(), QString::fromStdString(sdp));
        break;
    }

    case PendingOperation::SetLocalAnswer: {
        auto description = m_connection->local_description();
        std::string sdp;
        if (description != nullptr && description->ToString(&sdp))
            emit answerCreated(m_peerId, linkId(), QString::fromStdString(sdp));
        break;
    }

    case PendingOperation::SetRemoteOffer:
        // 本端为应答方：对端的 offer 已就位，现由本端生成 answer
        m_remoteDescriptionSet = true;
        flushPendingCandidates();
        createAnswer();
        break;

    case PendingOperation::SetRemoteAnswer:
        // 这是发起方：answer 已就位，协商完成，等着 ICE 打通。
        // ICE restart 到这里才算真正完成（offer 发出去但 answer 失败的
        // 情况由 OnFailure 重新排队）。
        m_iceRestartOffer.store(false);
        m_remoteDescriptionSet = true;
        flushPendingCandidates();
        break;

    case PendingOperation::None:
        break;
    }
}

void PeerLink::createAnswer() {
    if (m_connection == nullptr)
        return;

    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    m_pendingOperation = PendingOperation::SetLocalAnswer;
    m_connection->CreateAnswer(this, options);
}

void PeerLink::applyVideoQuality(int maxBitrateBps, double maxFramerate) {
    m_videoMaxBitrateBps = maxBitrateBps;
    m_videoMaxFramerate = maxFramerate;
    pushVideoQuality();
}

void PeerLink::pushVideoQuality() {
    if (m_videoSender == nullptr || m_connection == nullptr)
        return;

    // SetParameters 要求在信令线程上调用。BlockingCall 在"本来就在该线程"时
    // 会直接执行，不会自锁，所以这里不用自己判断线程。
    webrtc::Thread *signaling = m_connection->signaling_thread();
    if (signaling == nullptr)
        return;

    signaling->BlockingCall([this]() {
        webrtc::RtpParameters parameters = m_videoSender->GetParameters();
        if (parameters.encodings.empty())
            return;

        // 只有一路编码（不做 simulcast）,所以改 encodings[0] 即可。
        //
        // 三个旋钮的含义：
        //   max_bitrate_bps  码率上限。**是上限不是固定值** —— 实际码率仍由
        //                    拥塞控制在这个上限内自适应。
        //   max_framerate    帧率上限。观影场景很划算：电影本来 24fps,
        //                    网络差时降到 15 肉眼几乎无感，码率却能省一半。
        //   分辨率           不在这里设 —— 让编码器按码率自己降(过载时它会
        //                    先降分辨率），比自行推测缩放系数更准确。
        webrtc::RtpEncodingParameters &encoding = parameters.encodings[0];

        if (m_videoMaxBitrateBps > 0)
            encoding.max_bitrate_bps = m_videoMaxBitrateBps;
        else
            encoding.max_bitrate_bps.reset();

        if (m_videoMaxFramerate > 0)
            encoding.max_framerate = m_videoMaxFramerate;
        else
            encoding.max_framerate.reset();

        const webrtc::RTCError error = m_videoSender->SetParameters(parameters);
        if (!error.ok()) {
            LOG_WARN("PeerLink") << m_peerId << "设置推送画质失败:" << error.message();
            return;
        }

        LOG_INFO("PeerLink") << m_peerId << "推送画质已设置: 码率上限"
                             << (m_videoMaxBitrateBps > 0
                                     ? QString::number(m_videoMaxBitrateBps / 1000) + "kbps"
                                     : QStringLiteral("默认"))
                             << "帧率上限"
                             << (m_videoMaxFramerate > 0
                                     ? QString::number(m_videoMaxFramerate) + "fps"
                                     : QStringLiteral("默认"));
    });
}

void PeerLink::applyMovieAudioSending(bool active) {
    if (m_movieAudioSending == active)
        return;

    m_movieAudioSending = active;
    // 建连前 sender 还不存在，push 会直接返回；连接建立后由
    // OnIceConnectionChange / OnConnectionChange 补一次。
    pushMovieAudioSending();
}

void PeerLink::pushMovieAudioSending() {
    if (m_movieAudioSender == nullptr || m_connection == nullptr)
        return;

    // SetParameters 要求在信令线程上调用。BlockingCall 在"本来就在该线程"时
    // 会直接执行，不会自锁。
    webrtc::Thread *signaling = m_connection->signaling_thread();
    if (signaling == nullptr)
        return;

    const bool active = m_movieAudioSending;
    signaling->BlockingCall([this, active]() {
        if (m_movieAudioSender == nullptr)
            return;

        webrtc::RtpParameters parameters = m_movieAudioSender->GetParameters();
        if (parameters.encodings.empty()) {
            LOG_DEBUG("PeerLink") << m_peerId << "电影音轨 sender 没有 encoding，"
                                                 "无法设置发送状态";
            return;
        }

        // **不能因为缓存里已经是目标值就跳过**：协商前 SetParameters 写的是
        // 当时的 sender 参数，随后 SetLocalDescription/SetRemoteDescription
        // 会把发送流重新启动起来，而缓存里的 active 仍是旧值。必须在连接建立
        // 后真正再调用一次，UpdateSendState() 才会执行 Stop()。
        parameters.encodings[0].active = active;

        const webrtc::RTCError error = m_movieAudioSender->SetParameters(parameters);
        if (!error.ok()) {
            // 协商完成前 SendStream 尚未建立，这里会失败；OnConnectionChange
            // 中会在连上之后补一次。连上后仍失败才是真正的问题。
            LOG_DEBUG("PeerLink") << m_peerId << "设置电影音轨发送状态失败(目标"
                                  << (active ? "开" : "关") << "):"
                                  << error.message();
            return;
        }

        LOG_INFO("PeerLink") << m_peerId << (active ? "恢复发送电影音轨"
                                                    : "停止发送电影音轨");
    });
}

void PeerLink::applyVideoSending(bool active) {
    if (m_videoSending == active)
        return;

    m_videoSending = active;
    // 建连前 sender 还不存在，push 会直接返回；连接建立后由
    // OnIceConnectionChange / OnConnectionChange 补一次。
    pushVideoSending();
}

void PeerLink::pushVideoSending() {
    if (m_videoSender == nullptr || m_connection == nullptr)
        return;

    webrtc::Thread *signaling = m_connection->signaling_thread();
    if (signaling == nullptr)
        return;

    const bool active = m_videoSending;
    signaling->BlockingCall([this, active]() {
        if (m_videoSender == nullptr)
            return;

        webrtc::RtpParameters parameters = m_videoSender->GetParameters();
        if (parameters.encodings.empty()) {
            LOG_DEBUG("PeerLink") << m_peerId << "视频 sender 没有 encoding，"
                                                 "无法设置发送状态";
            return;
        }

        // 与电影音轨同样的理由：协商会重新启动发送流，连上后必须再写一次。
        parameters.encodings[0].active = active;

        const webrtc::RTCError error = m_videoSender->SetParameters(parameters);
        if (!error.ok()) {
            LOG_DEBUG("PeerLink") << m_peerId << "设置视频发送状态失败(目标"
                                  << (active ? "开" : "关") << "):"
                                  << error.message();
            return;
        }

        LOG_INFO("PeerLink") << m_peerId << (active ? "恢复发送视频"
                                                    : "停止发送视频");
    });
}

// ============================
// 数据通道
// ============================

PeerLink::ChannelSlot &PeerLink::slotFor(DataChannelKind kind) {
    return kind == DataChannelKind::Control ? m_controlChannel : m_dataChannel;
}

const PeerLink::ChannelSlot &PeerLink::slotFor(DataChannelKind kind) const {
    return kind == DataChannelKind::Control ? m_controlChannel : m_dataChannel;
}

PeerLink::ChannelObserver *PeerLink::observerFor(DataChannelKind kind) const {
    return kind == DataChannelKind::Control ? m_controlObserver.get()
                                            : m_dataObserver.get();
}

QString PeerLink::channelId(DataChannelKind kind) {
    return kind == DataChannelKind::Control
               ? QString::fromLatin1(TransferChannel::ControlId)
               : QString::fromLatin1(TransferChannel::DataId);
}

std::optional<PeerLink::DataChannelKind> PeerLink::kindFromId(const QString &channel) {
    if (channel == QLatin1String(TransferChannel::ControlId))
        return DataChannelKind::Control;
    if (channel == QLatin1String(TransferChannel::DataId))
        return DataChannelKind::Data;
    return std::nullopt;
}

std::optional<PeerLink::DataChannelKind> PeerLink::kindFromLabel(const std::string &label) {
    if (label == TransferChannel::ControlLabel)
        return DataChannelKind::Control;
    if (label == TransferChannel::DataLabel)
        return DataChannelKind::Data;
    return std::nullopt;
}

void PeerLink::attachDataChannel(
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel,
    DataChannelKind kind) {
    if (channel == nullptr)
        return;

    bool accepted = false;
    bool shouldClose = false;
    {
        webrtc::MutexLock lock(&m_dataChannelLock);
        ChannelSlot &slot = slotFor(kind);
        if (m_dataChannelClosing) {
            // 连接已在拆除流程中，别再挂上来
            shouldClose = true;
        } else if (slot.channel == nullptr) {
            slot.channel = channel;
            slot.openReported = false;
            slot.closedReported = false;
            accepted = true;
        } else if (slot.channel.get() != channel.get()) {
            // 同一路只认一条通道（旧版对端可能重复创建）
            LOG_WARN("PeerLink") << m_peerId << "忽略重复的数据通道:"
                                 << channel->label();
            shouldClose = true;
        }
    }

    if (shouldClose) {
        channel->Close();
        return;
    }
    if (!accepted)
        return;

    channel->RegisterObserver(observerFor(kind));

    // 注册期间连接可能已经开始拆除（close() 并发进来）。这种情况下
    // 取消注册，避免回调落在已经作废的通道上。
    {
        webrtc::MutexLock lock(&m_dataChannelLock);
        if (m_dataChannelClosing || slotFor(kind).channel.get() != channel.get()) {
            channel->UnregisterObserver();
            return;
        }
    }

    // 注册时通道可能已经打开，OnStateChange 不会补发，这里主动对一次状态
    onDataChannelStateChanged(kind);
}

void PeerLink::onDataChannelStateChanged(DataChannelKind kind) {
    webrtc::scoped_refptr<webrtc::DataChannelInterface> channel;
    {
        webrtc::MutexLock lock(&m_dataChannelLock);
        channel = slotFor(kind).channel;
    }
    if (channel == nullptr)
        return;

    const webrtc::DataChannelInterface::DataState state = channel->state();
    if (state == webrtc::DataChannelInterface::kOpen) {
        bool reported = false;
        {
            webrtc::MutexLock lock(&m_dataChannelLock);
            ChannelSlot &slot = slotFor(kind);
            if (slot.channel == channel && !slot.openReported) {
                slot.openReported = true;
                slot.closedReported = false;
                reported = true;
            }
        }
        if (reported) {
            LOG_INFO("PeerLink") << (m_peerId + ":" + linkId())
                                 << "数据通道已打开:" << channelId(kind);
            emit dataChannelOpened(m_peerId, channelId(kind));
        }
        return;
    }

    if (state == webrtc::DataChannelInterface::kClosing
        || state == webrtc::DataChannelInterface::kClosed) {
        bool reported = false;
        {
            webrtc::MutexLock lock(&m_dataChannelLock);
            ChannelSlot &slot = slotFor(kind);
            if (slot.channel == channel && !slot.closedReported) {
                slot.closedReported = true;
                slot.openReported = false;
                reported = true;
            }
        }
        if (reported) {
            LOG_INFO("PeerLink") << (m_peerId + ":" + linkId())
                                 << "数据通道已关闭:" << channelId(kind);
            emit dataChannelClosed(m_peerId, channelId(kind));
        }
    }
}

void PeerLink::onDataChannelMessage(DataChannelKind kind,
                                    const webrtc::DataBuffer &buffer) {
    // 到主线程之前就把数据拷出来：buffer 的内存在回调返回后失效
    const QByteArray data(reinterpret_cast<const char *>(buffer.data.data()),
                          static_cast<int>(buffer.data.size()));
    emit dataChannelMessageReceived(m_peerId, channelId(kind), buffer.binary, data);
}

bool PeerLink::isDataChannelOpen(const QString &channel) const {
    const std::optional<DataChannelKind> kind = kindFromId(channel);
    if (!kind.has_value())
        return false;

    webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel;
    {
        webrtc::MutexLock lock(&m_dataChannelLock);
        dataChannel = slotFor(*kind).channel;
    }
    return dataChannel != nullptr
           && dataChannel->state() == webrtc::DataChannelInterface::kOpen;
}

quint64 PeerLink::dataChannelBufferedAmount(const QString &channel) const {
    const std::optional<DataChannelKind> kind = kindFromId(channel);
    if (!kind.has_value())
        return 0;

    webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel;
    {
        webrtc::MutexLock lock(&m_dataChannelLock);
        dataChannel = slotFor(*kind).channel;
    }
    return dataChannel != nullptr ? dataChannel->buffered_amount() : 0;
}

bool PeerLink::sendDataChannelMessage(const QString &channel, const QByteArray &data,
                                      bool binary) {
    const std::optional<DataChannelKind> kind = kindFromId(channel);
    if (!kind.has_value())
        return false;

    webrtc::scoped_refptr<webrtc::DataChannelInterface> dataChannel;
    {
        webrtc::MutexLock lock(&m_dataChannelLock);
        dataChannel = slotFor(*kind).channel;
    }
    if (dataChannel == nullptr
        || dataChannel->state() != webrtc::DataChannelInterface::kOpen) {
        return false;
    }

    webrtc::DataBuffer buffer(
        webrtc::CopyOnWriteBuffer(reinterpret_cast<const uint8_t *>(data.constData()),
                                  static_cast<size_t>(data.size())),
        binary);
    return dataChannel->Send(buffer);
}

void PeerLink::requestDataChannelStats() {
    if (m_connection == nullptr)
        return;

    // 每次新建回调对象：GetStats 是异步的，回调可能在本对象析构之后才到。
    // StatsLogger 只持有 peerId/link 字符串，不碰 this。
    auto callback = webrtc::make_ref_counted<StatsLogger>(m_peerId, linkId());
    m_connection->GetStats(callback.get());
}

void PeerLink::emitError(const QString &message) {
    LOG_ERROR("PeerLink") << m_peerId << message;
    emit errorOccurred(m_peerId, linkId(), message);
}
