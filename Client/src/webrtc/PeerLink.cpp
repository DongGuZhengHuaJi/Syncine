//
// Created by donggu on 2026/9/14.
//

#include "PeerLink.h"

#include <iostream>

#include "WebrtcManager.h"



// ============================
// 构造 / 销毁
// ============================

PeerLink::PeerLink(const QString &peerId,
                   webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory,
                   const webrtc::PeerConnectionInterface::RTCConfiguration &config,
                   bool createDataChannel,
                   QObject *parent)
    : QObject(parent),
      m_peerId(peerId),
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
            std::cerr << "[PeerLink " << m_peerId.toStdString()
                      << "] 创建数据通道失败: " << channel.error().message() << std::endl;
        }
    }
}

void PeerLink::close() {
    m_pendingCandidateSdps.clear();
    m_pendingCandidateMids.clear();
    m_pendingCandidateIndexes.clear();
    m_remoteDescriptionSet = false;
    m_pendingOperation = PendingOperation::None;

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
        std::cerr << "[PeerLink " << m_peerId.toStdString()
                  << "] 上一次协商还没结束,忽略重复的 createOffer" << std::endl;
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

    // 已经挂过同一根音轨就直接返回。
    if (m_localAudioTrack != nullptr
        && m_localAudioTrack->id() == track->id()) {
        return;
    }


    // 在createOffer之前挂音轨,SDP中会包含相应的m=audio段
    auto sender = m_connection->AddTrack(track, {"syncine-audio"});
    if (!sender.ok()) {
        std::cerr << "[PeerLink " << m_peerId.toStdString()
                  << "] 添加音轨失败: " << sender.error().message() << std::endl;
        return;
    }

    m_localAudioTrack = track;

    std::cout << "[PeerLink " << m_peerId.toStdString() << "] 已挂载本地音轨" << std::endl;
}

void PeerLink::addLocalVideoTrack(webrtc::scoped_refptr<webrtc::VideoTrackInterface> track) {
    if (m_connection == nullptr || track == nullptr)
        return;

    // 已经挂过同一根视频轨就直接返回。
    if (m_localVideoTrack != nullptr
        && m_localVideoTrack->id() == track->id()) {
        return;
    }


    // 在createOffer之前挂视频轨,SDP中会包含相应的m=video段
    auto sender = m_connection->AddTrack(track, {"syncine-video"});
    if (!sender.ok()) {
        std::cerr << "[PeerLink " << m_peerId.toStdString()
                  << "] 添加视频轨失败: " << sender.error().message() << std::endl;
        return;
    }

    m_localVideoTrack = track;

    std::cout << "[PeerLink " << m_peerId.toStdString() << "] 已挂载本地视频轨" << std::endl;
}

void PeerLink::setAudioMuted(bool muted) {
    if (m_localAudioTrack == nullptr)
        return;

    m_localAudioTrack->set_enabled(!muted);
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
    std::cout << "[PeerLink " << m_peerId.toStdString() << "] 收到数据通道: "
              << channel->label() << std::endl;
    emit connected(m_peerId);
}

void PeerLink::OnRenegotiationNeeded() {
}

void PeerLink::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) {
    switch (state) {
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionConnected:
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionCompleted:
        emit connected(m_peerId);
        break;
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionFailed:
        emitError(QStringLiteral("ICE 连接失败(双方可能不在同一网络)"));
        break;
    case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionDisconnected:
        std::cout << "[PeerLink " << m_peerId.toStdString() << "] ICE 断开" << std::endl;
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

    std::cout << "[PeerLink " << m_peerId.toStdString()
              << "] 收到远端媒体轨: " << track->kind() << std::endl;

    // 只处理视频轨，音频轨交给webrtc处理
    if (track->kind() != webrtc::MediaStreamTrackInterface::kVideoKind)
        return;

    auto videoTrack = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(
        static_cast<webrtc::VideoTrackInterface *>(track.get()));

    // 保留引用避免被释放
    m_remoteVideoTrack = videoTrack;

    // 检查是否有视频渲染器
    if (m_remoteVideoSink == nullptr) {
        std::cout << "[PeerLink " << m_peerId.toStdString()
                  << "] 收到视频轨,但没有渲染器可接(推流接收未启用)" << std::endl;
        return;
    }

    // 将远端视频轨接到渲染器上
    m_remoteVideoTrack->AddOrUpdateSink(m_remoteVideoSink, webrtc::VideoSinkWants{});
    std::cout << "[PeerLink " << m_peerId.toStdString()
              << "] 远端视频已接到渲染器" << std::endl;
}

void PeerLink::setRemoteVideoSink(webrtc::VideoSinkInterface<webrtc::VideoFrame> *sink) {
    m_remoteVideoSink = sink;
}

void PeerLink::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) {
    switch (state) {
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
        emit connected(m_peerId);
        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
        emitError(QStringLiteral("连接失败"));
        break;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
        emit closed(m_peerId);
        break;
    default:
        break;
    }
}

void PeerLink::OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState) {
}

void PeerLink::OnIceCandidateError(const std::string &address, int port, const std::string &url,
                                   int error_code, const std::string &error_text) {
    std::cerr << "[PeerLink " << m_peerId.toStdString() << "] ICE candidate 出错: "
              << url << " " << error_code << " " << error_text
              << " (地址 " << address << ":" << port << ")" << std::endl;
}

// ---- CreateOffer / CreateAnswer 返回 ----

void PeerLink::OnSuccess(webrtc::SessionDescriptionInterface *desc) {
    if (desc == nullptr || m_connection == nullptr) {
        emitError(QStringLiteral("协商返回了空描述"));
        return;
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
            emit offerCreated(m_peerId, QString::fromStdString(sdp));
        break;
    }

    case PendingOperation::SetLocalAnswer: {
        auto description = m_connection->local_description();
        std::string sdp;
        if (description != nullptr && description->ToString(&sdp))
            emit answerCreated(m_peerId, QString::fromStdString(sdp));
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
    std::cerr << "[PeerLink " << m_peerId.toStdString() << "] " << message.toStdString() << std::endl;
    emit errorOccurred(m_peerId, message);
}
