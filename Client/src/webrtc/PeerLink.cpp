//
// Created by donggu on 2026/9/14.
//

#include "PeerLink.h"

#include <optional>
#include <sstream>
#include <string>

#include "WebrtcManager.h"
#include "core/Log.h"

namespace {

// 给一条音轨设音量。
//
// **必须在 WebRTC 信令线程上执行** —— track->GetSource()->SetVolume() 最终会走到
// AudioRtpReceiver::OnSetVolume,那里有 RTC_DCHECK_RUN_ON(&signaling_thread_checker_)。
//
// 单独拆成自由函数是为了让投递到信令线程的 lambda 只捕获 track 本身
// (scoped_refptr 会保住它的命),而**不捕获 this** —— 否则 PeerLink 析构后
// 任务才轮到执行,就会踩到悬空指针。
void setSourceVolume(const webrtc::scoped_refptr<webrtc::AudioTrackInterface> &track,
                     double volume) {
    if (track == nullptr)
        return;

    webrtc::AudioSourceInterface *source = track->GetSource();
    if (source != nullptr)
        source->SetVolume(volume);
}

// 把一条轨上挂着的所有 MediaStream id 拼起来,排障用。
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

} // namespace


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

    webrtc::PeerConnectionDependencies dependencies(this);
    auto result = m_factory->CreatePeerConnectionOrError(config, std::move(dependencies));
    if (!result.ok()) {
        emitError(QStringLiteral("创建 PeerConnection 失败: ")
                  + QString::fromStdString(result.error().message()));
        return;
    }
    m_connection = result.value();

    // 建一条数据通道，暂时没有数据传输，只是验证链路联通性
    if (createDataChannel) {
        webrtc::DataChannelInit init;
        init.ordered = true;
        auto channel = m_connection->CreateDataChannelOrError("syncine-probe", &init);
        if (!channel.ok()) {
            LOG_ERROR("PeerLink") << m_peerId << "创建数据通道失败:"
                                  << channel.error().message();
        }
    }
}

void PeerLink::close() {
    m_pendingCandidateSdps.clear();
    m_pendingCandidateMids.clear();
    m_pendingCandidateIndexes.clear();
    m_remoteDescriptionSet = false;
    m_pendingOperation = PendingOperation::None;

    // 立刻松开外部播放器。
    //
    // 这个指针指向 WebrtcManager 持有的 MovieAudioPlayer。管理器的 destroy()
    // 里是先关连接、再放播放器,而关闭到真正析构之间还有一段 deleteLater 的
    // 窗口期 —— 只要这期间还有一帧音频摸进来,就会踩到已经释放的播放器。
    // 这里先断开,窗口就不存在了。
    m_remoteAudioSink = nullptr;

    if (m_connection != nullptr) {
        // 先断开连接再释放,防止析构过程中发生回调
        m_connection->Close();
        m_connection = nullptr;
    }
    m_factory = nullptr;
}

// ============================
// 信令入站
// ============================

void PeerLink::createOffer() {
    if (m_connection == nullptr) {
        emitError(QStringLiteral("连接未建立,无法发起协商"));
        return;
    }
    if (m_pendingOperation != PendingOperation::None) {
        LOG_WARN("PeerLink") << m_peerId << "上一次协商还没结束,忽略重复的 createOffer";
        return;
    }

    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    m_pendingOperation = PendingOperation::SetLocalOffer;
    m_connection->CreateOffer(this, options);
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

    // 远端描述没设好之前,AddIceCandidate 会直接失败。而 ICE 与 SDP 是
    // 两条独立通道,候选完全可能先到(尤其是局域网,候选几乎瞬间产生)。
    // 所以先存队列,等 SetRemoteDescription 成功后再补投。
    if (!m_remoteDescriptionSet) {
        m_pendingCandidateSdps.append(sdp);
        m_pendingCandidateMids.append(sdpMid);
        m_pendingCandidateIndexes.append(sdpMLineIndex);
        return;
    }

    webrtc::SdpParseError parseError;
    // 这个重载返回裸指针(所有权归调用方),要自己接管
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


    // 在createOffer之前挂音轨,SDP中会包含相应的m=audio段
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

void PeerLink::addLocalVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track) {
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


    // 在createOffer之前挂视频轨,SDP中会包含相应的m=video段
    auto sender = m_connection->AddTrack(track, {"syncine-video"});
    if (!sender.ok()) {
        LOG_ERROR("PeerLink") << m_peerId << "添加视频轨失败:" << sender.error().message();
        return;
    }

    m_localVideoTrack = track;

    LOG_INFO("PeerLink") << m_peerId << "已挂载本地视频轨";
}

void PeerLink::addLocalMovieAudioTrack(webrtc::scoped_refptr<webrtc::AudioTrackInterface> track) {
    if (m_connection == nullptr || track == nullptr)
        return;

    if (m_kind != LinkKind::Media) {
        LOG_WARN("PeerLink") << (m_peerId + ":" + linkId())
                             << "电影音轨只能在 media 连接挂载,已忽略";
        return;
    }

    if (m_localMovieAudioTrack != nullptr
        && m_localMovieAudioTrack->id() == track->id()) {
        return;
    }

    // stream id 用 kMovieStreamId —— 接收端就是靠它把这条轨认成"电影"的。
    auto sender = m_connection->AddTrack(track, {kMovieStreamId});
    if (!sender.ok()) {
        LOG_ERROR("PeerLink") << m_peerId << "添加电影音轨失败:" << sender.error().message();
        return;
    }

    m_localMovieAudioTrack = track;

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

    // 1. 记下目标音量(轨还没到也要记 —— OnTrack 到达时会套用),
    //    同时把轨的引用抄一份出来,免得下面投递任务期间被换掉。
    {
        webrtc::MutexLock lock(&m_remoteAudioLock);
        m_remoteVolume = volume;
        track = m_remoteAudioTrack;
    }

    // 连接已经没了:音量已经记下,等下次 OnTrack 自己套用。
    if (m_connection == nullptr || track == nullptr)
        return;

    webrtc::Thread *signalingThread = m_connection->signaling_thread();
    if (signalingThread == nullptr)
        return;

    // 已经在信令线程上就直接设,省一轮投递。
    // (正常不会走到这里 —— 这个接口是给 Qt 主线程的音量滑块调的)
    if (signalingThread->IsCurrent()) {
        setSourceVolume(track, volume);
        return;
    }

    // 2. 投递到信令线程执行。lambda 只捕获 track 和音量值,**不捕获 this**,
    //    这样即使 PeerLink 先析构、任务后执行,也不会踩到悬空指针。
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

    // 数据通道打通后输出
    LOG_INFO("PeerLink") << m_peerId << "收到数据通道:" << channel->label();
    emit connected(m_peerId, linkId());
}

void PeerLink::OnRenegotiationNeeded() {
}

void PeerLink::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) {
    switch (state) {
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionConnected:
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionCompleted:
        emit connected(m_peerId, linkId());
        break;
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionFailed:
        emitError(QStringLiteral("ICE 连接失败(双方可能不在同一网络)"));
        break;
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionDisconnected:
        LOG_WARN("PeerLink") << m_peerId << "ICE 断开";
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

    // 把能用来"认出这是哪条轨"的线索都打出来,排障用。
    // 注意 track->id() 是 WebRTC 随机生成的 UUID,认不出是谁;
    // 不过现在也不需要认了 —— 轨是从哪条连接来的,它就是什么。
    LOG_INFO("PeerLink") << (m_peerId + ":" + linkId()) << "收到远端媒体轨:"
                         << track->kind()
                         << "(mid=" << transceiver->mid().value_or("<无>")
                         << ", streams=[" << describeStreams(receiver) << "])";

    // ---- 音频轨 ----
    //
    // 每条连接只有一条音频轨,所以它的身份由 m_kind 直接决定,不用猜:
    //   Voice → 对端的麦克风。放音交给 WebRTC 自己(真声卡),我们只管音量。
    //   Media → 对端的电影声。假声卡放不出来,交给我们自己的 sink 播。
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
            // 自己播的那条(电影):把轨接到我们的 sink 上,由 MovieAudioPlayer
            // 解码后的 PCM 喂给 QAudioSink。音量也在那边管。
            audioTrack->AddSink(m_remoteAudioSink);
            LOG_INFO("PeerLink") << (m_peerId + ":" + linkId()) << "远端音轨已接到本地播放器";
        } else {
            // 交给 WebRTC 放的那条(语音)。本回调就在信令线程上,
            // 正好满足 SetVolume 的线程要求,顺手把当前音量套上。
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
    LOG_ERROR("PeerLink") << m_peerId << "ICE candidate 出错:" << url << error_code
                          << error_text << "(地址" << address << ":" << port << ")";
}

// ---- CreateOffer / CreateAnswer 返回 ----

void PeerLink::OnSuccess(webrtc::SessionDescriptionInterface *desc) {
    if (desc == nullptr || m_connection == nullptr) {
        emitError(QStringLiteral("协商返回了空描述"));
        return;
    }

    // 诊断:把本地 SDP 里和 Opus 有关的行打出来。音频声道问题排查全靠它 ——
    // fmtp 里有没有 stereo=1,一眼就能看出这条连接的协商是不是立体声
    // (rtpmap 里的 /2 只是能力声明,不算数 —— 见 WebrtcManager.cpp 的
    // StereoOpusAudioEncoderFactory 注释)。
    {
        std::string sdp;
        if (desc->ToString(&sdp)) {
            std::istringstream lines(sdp);
            std::string line;
            while (std::getline(lines, line)) {
                if (line.find("opus") != std::string::npos)
                    LOG_TRACE("PeerLink") << m_peerId << "SDP:" << line;
            }
        }
    }

    // 本地描述必须落下去,后续的 SetRemoteDescription 才有基准。
    // OnSuccess() 无参重载会在它成功后被回调。
    m_connection->SetLocalDescription(this, desc);
}

void PeerLink::OnFailure(webrtc::RTCError error) {
    m_pendingOperation = PendingOperation::None;
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
        // 这是应答方:对端的 offer 已就位,该我们生成 answer 了
        m_remoteDescriptionSet = true;
        flushPendingCandidates();
        createAnswer();
        break;

    case PendingOperation::SetRemoteAnswer:
        // 这是发起方:answer 已就位,协商完成,等着 ICE 打通
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

void PeerLink::emitError(const QString &message) {
    LOG_ERROR("PeerLink") << m_peerId << message;
    emit errorOccurred(m_peerId, linkId(), message);
}
