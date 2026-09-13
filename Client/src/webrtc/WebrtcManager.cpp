//
// Created by donggu on 2026/9/10.
//

#include "WebrtcManager.h"

#include <iostream>

#include "rtc_base/checks.h"

// ----------------------------------------------------------------------
// ABI 护栏:客户端看到的类布局必须和 libwebrtc.a 编译时的一致。
//
// WebRTC 头文件里存在 `#if RTC_DCHECK_IS_ON` 包裹的条件成员
// (见 rtc_base/thread.h 中 webrtc::Thread 的字段声明)。这个宏两边不一致时,
// webrtc::Thread 之类的类在客户端和库里 sizeof 不同、成员偏移整体错位,
// 症状是运行时随机段错误,极难定位。
//
// libwebrtc.a 为 Release 构建(is_debug = false),RTC_DCHECK_IS_ON 必须为 0;
// 由 Client/CMakeLists.txt 第 7 节的 NDEBUG 保证。这里编译失败时:
//   - 若你确实把 WebRTC 重编成了带 DCHECK 的版本,请同步去掉那边的 NDEBUG
//   - 否则就是 NDEBUG 被误删了,加回去
// ----------------------------------------------------------------------
#if RTC_DCHECK_IS_ON
#error "RTC_DCHECK_IS_ON 应为 0:libwebrtc.a 是 Release(is_debug = false)构建,详见 Client/CMakeLists.txt 第 7 节"
#endif

WebrtcManager::WebrtcManager() {
}

WebrtcManager::~WebrtcManager() {
    destroy();
}

bool WebrtcManager::initialize(const std::vector<std::string> &stunServers) {
    if (m_initialized) {
        return true;
    }
    if (!initializeThreads()) {
        return false;
    }
    if (!initializeFactory()) {
        destroy();
        return false;
    }
    m_initialized = true;
    m_stunServers = stunServers;
    return true;
}

bool WebrtcManager::createPeerConnection() {
    if (!m_initialized) {
        std::cerr << "WebrtcManager is not initialized" << std::endl;
        return false;
    }

    if (m_peerConnection) {
        std::cerr << "PeerConnection already exists" << std::endl;
        return true;
    }

    webrtc::PeerConnectionInterface::RTCConfiguration config;

    for (const auto &stunServer : m_stunServers) {
        webrtc::PeerConnectionInterface::IceServer iceServer;
        iceServer.urls.push_back(stunServer);
        config.servers.push_back(iceServer);
    }

    webrtc::PeerConnectionDependencies dependencies(this);
    auto result = m_peerConnectionFactory->CreatePeerConnectionOrError(config, std::move(dependencies));
    if (!result.ok()) {
        std::cerr << "Error creating PeerConnection: " << result.error().message() << std::endl;
        return false;
    }
    m_peerConnection = result.value();
    return true;
}

bool WebrtcManager::createOffer() {
    if (!m_peerConnection) {
        std::cerr << "PeerConnection is not created" << std::endl;
        return false;
    }
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    m_pendingOperation = PendingOperation::SetLocalOffer;
    m_peerConnection->CreateOffer(this, options);
    return true;
}

bool WebrtcManager::createAnswer() {
    if (!m_peerConnection) {
        std::cerr << "PeerConnection is not created" << std::endl;
        return false;
    }
    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
    m_pendingOperation = PendingOperation::SetLocalAnswer;
    m_peerConnection->CreateAnswer(this, options);
    return true;
}

bool WebrtcManager::setRemoteDescription(const std::string &sdp, const std::string &type) {
    if (!m_peerConnection) {
        std::cerr << "PeerConnection is not created" << std::endl;
        return false;
    }

    webrtc::SdpType sdpType;
    if (type == "offer") {
        sdpType = webrtc::SdpType::kOffer;
    } else if (type == "answer") {
        sdpType = webrtc::SdpType::kAnswer;
    } else {
        std::cerr << "Invalid SDP type: " << type << std::endl;
        return false;
    }

    webrtc::SdpParseError error;

    auto sessionDescription = webrtc::CreateSessionDescription(sdpType, sdp, &error);
    if (!sessionDescription) {
        std::cerr << "Error creating SessionDescription: " << error.description << std::endl;
        return false;
    }

    if (sdpType == webrtc::SdpType::kOffer) {
        m_pendingOperation = PendingOperation::SetRemoteOffer;
    } else if (sdpType == webrtc::SdpType::kAnswer) {
        m_pendingOperation = PendingOperation::SetRemoteAnswer;
    }

    m_peerConnection->SetRemoteDescription(this, sessionDescription.release());
    return true;
}

bool WebrtcManager::addIceCandidate(const std::string &sdp, const std::string &sdpMid, int sdpMLineIndex) {
    if (!m_peerConnection) {
        std::cerr << "PeerConnection is not created" << std::endl;
        return false;
    }

    webrtc::SdpParseError error;

    auto iceCandidate = webrtc::CreateIceCandidate(sdpMid, sdpMLineIndex, sdp, &error);
    if (!iceCandidate) {
        std::cerr << "Error creating IceCandidate: " << error.description << std::endl;
        return false;
    }

    if (!m_peerConnection->AddIceCandidate(iceCandidate)) {
        std::cerr << "Failed to add IceCandidate" << std::endl;
        return false;
    }

    return true;
}

void WebrtcManager::destroy() {
    m_pendingOperation = PendingOperation::None;
    if (m_peerConnection) {
        m_peerConnection->Close();
        m_peerConnection = nullptr;
    }
    if (m_peerConnectionFactory) {
        m_peerConnectionFactory = nullptr;
    }

    // 工厂释放后再停线程(工厂内部引用这些线程),否则析构运行中的线程会崩溃
    if (m_networkThread) {
        m_networkThread->Stop();
        m_networkThread.reset();
    }
    if (m_workerThread) {
        m_workerThread->Stop();
        m_workerThread.reset();
    }
    if (m_signalingThread) {
        m_signalingThread->Stop();
        m_signalingThread.reset();
    }

    m_initialized = false;
}

void WebrtcManager::OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) {
}

void WebrtcManager::OnAddStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
    PeerConnectionObserver::OnAddStream(stream);
}

void WebrtcManager::OnRemoveStream(webrtc::scoped_refptr<webrtc::MediaStreamInterface> stream) {
    PeerConnectionObserver::OnRemoveStream(stream);
}

void WebrtcManager::OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
}

void WebrtcManager::OnRenegotiationNeeded() {
    PeerConnectionObserver::OnRenegotiationNeeded();
}

void WebrtcManager::OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState ice_connection_state) {
    switch (ice_connection_state) {
        case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionNew:
            std::cout << "ICE connection state: New" << std::endl;
            break;
        case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionChecking:
            std::cout << "ICE connection state: Checking" << std::endl;
            break;
        case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionConnected:
            std::cout << "ICE connection state: Connected" << std::endl;
            break;
        case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionCompleted:
            std::cout << "ICE connection state: Completed" << std::endl;
            break;
        case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionFailed:
            std::cout << "ICE connection state: Failed" << std::endl;
            break;
        case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionDisconnected:
            std::cout << "ICE connection state: Disconnected" << std::endl;
            break;
        case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionClosed:
            std::cout << "ICE connection state: Closed" << std::endl;
            break;
        default:
            std::cerr << "ICE connection state: Unknown" << std::endl;
            break;
    }
    PeerConnectionObserver::OnIceConnectionChange(ice_connection_state);
}

void WebrtcManager::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) {
}

void WebrtcManager::OnIceCandidate(const webrtc::IceCandidate *candidate) {
    if (!candidate) {
        std::cerr << "OnIceCandidate called with null candidate" << std::endl;
        return;
    }

    std::string sdp;
    if (!candidate->ToString(&sdp)) {
        std::cerr << "Failed to convert IceCandidate to string" << std::endl;
        return;
    }
    std::cout << "New ICE candidate: " << sdp << std::endl;

    emit iceCandidateCreated(QString::fromStdString(sdp),
                             QString::fromStdString(candidate->sdp_mid()),
                             candidate->sdp_mline_index());
}

void WebrtcManager::OnIceConnectionReceivingChange(bool cond) {
    PeerConnectionObserver::OnIceConnectionReceivingChange(cond);
}

void WebrtcManager::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {

}

void WebrtcManager::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState peer_connection_state) {
    switch (peer_connection_state) {
        case webrtc::PeerConnectionInterface::PeerConnectionState::kNew:
            std::cout << "PeerConnection state: New" << std::endl;
            break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting:
            std::cout << "PeerConnection state: Connecting" << std::endl;
            break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
            std::cout << "PeerConnection state: Connected" << std::endl;
            break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
            std::cout << "PeerConnection state: Disconnected" << std::endl;
            break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
            std::cout << "PeerConnection state: Failed" << std::endl;
            break;
        case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
            std::cout << "PeerConnection state: Closed" << std::endl;
            break;
    }
    PeerConnectionObserver::OnConnectionChange(peer_connection_state);
}

void WebrtcManager::OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) {
    PeerConnectionObserver::OnStandardizedIceConnectionChange(new_state);
}

void WebrtcManager::OnIceCandidateError(const std::string &basic_string, int i, const std::string &string, int i1,
    const std::string &basic_string1) {
    PeerConnectionObserver::OnIceCandidateError(basic_string, i, string, i1, basic_string1);
}


// Called when CreateOffer or CreateAnswer succeeds.
void WebrtcManager::OnSuccess(webrtc::SessionDescriptionInterface *desc) {
    if (desc == nullptr || !m_peerConnection) {
        std::cerr << "OnSuccess called with null desc or no peer connection" << std::endl;
        emit errorOccurred("CreateOffer/Answer 返回了空描述");
        return;
    }
    std::cout << "OnSuccess: " << desc->type() << std::endl;
    m_peerConnection->SetLocalDescription(this, desc);
}

void WebrtcManager::OnFailure(webrtc::RTCError error) {
    std::cerr << "OnFailure: " << error.message() << std::endl;
    m_pendingOperation = PendingOperation::None;
    emit errorOccurred(QString::fromStdString(error.message()));
}

// Called when SetLocalDescription or SetRemoteDescription succeeds.
void WebrtcManager::OnSuccess() {
    const auto operation = m_pendingOperation;
    std::string sdp;
    auto description = m_peerConnection->local_description();
    switch (operation) {
        case PendingOperation::SetRemoteOffer:
            std::cout << "SetRemoteOffer succeeded" << std::endl;

            m_pendingOperation = PendingOperation::None;
            createAnswer();
            break;
        case PendingOperation::SetRemoteAnswer:
            std::cout << "SetRemoteAnswer succeeded" << std::endl;

            m_pendingOperation = PendingOperation::None;
            break;
        case PendingOperation::SetLocalOffer:
            std::cout << "SetLocalOffer succeeded" << std::endl;

            if (description && description->ToString(&sdp)) {
                emit offerCreated(QString::fromStdString(sdp));
            }

            m_pendingOperation = PendingOperation::None;
            break;
        case PendingOperation::SetLocalAnswer:
            std::cout << "SetLocalAnswer succeeded" << std::endl;

            if (description && description->ToString(&sdp)) {
                emit answerCreated(QString::fromStdString(sdp));
            }

            m_pendingOperation = PendingOperation::None;
            break;
        case PendingOperation::None:
            std::cout << "SetDescription succeeded with no pending operation" << std::endl;
            break;
        default:
            std::cerr << "SetDescription succeeded with unknown pending operation" << std::endl;
            break;
    }
}

bool WebrtcManager::initializeThreads() {
    m_networkThread = webrtc::Thread::CreateWithSocketServer();
    m_signalingThread = webrtc::Thread::Create();
    m_workerThread = webrtc::Thread::Create();


    if (!m_networkThread->Start()) {
        std::cerr << "Error starting network thread" << std::endl;
        m_networkThread->Stop();
        m_networkThread.reset();
        return false;
    }
    if (!m_signalingThread->Start()) {
        std::cerr << "Error starting signaling thread" << std::endl;
        m_networkThread->Stop();
        m_networkThread.reset();
        m_signalingThread->Stop();
        m_signalingThread.reset();
        return false;
    }
    if (!m_workerThread->Start()) {
        std::cerr << "Error starting worker thread" << std::endl;
        m_networkThread->Stop();
        m_networkThread.reset();
        m_signalingThread->Stop();
        m_signalingThread.reset();
        m_workerThread->Stop();
        m_workerThread.reset();
        return false;
    }

    return true;
}

bool WebrtcManager::initializeFactory() {
    // 音频/视频编解码工厂必须真传进去,不能留 nullptr:
    // 旧版 CreatePeerConnectionFactory() 只是把它们原样搬进
    // PeerConnectionFactoryDependencies(见 api/create_peerconnection_factory.cc),
    // 并不做非空兜底。传 nullptr 会让 WebRtcVoiceEngine 拿到空的
    // encoder_factory_,随后在 encoder_factory_->GetSupportedEncoders()
    // 上空指针虚调用,直接 SIGSEGV —— 而且崩在 WebRTC 自己的线程里,
    // 栈上完全看不到调用方,极难定位。
    //
    // 这几个内置工厂的头文件在 WebrtcManager.h 里本就 include 了,之前漏用。
    // default_adm / audio_mixer / audio_processing 留 nullptr 是安全的:
    // 前两个 WebRTC 会按平台默认建,apm 走内置实现。
    m_peerConnectionFactory = webrtc::CreatePeerConnectionFactory(
        m_networkThread.get(),
        m_workerThread.get(),
        m_signalingThread.get(),
        /*default_adm=*/nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(),
        /*audio_mixer=*/nullptr,
        /*audio_processing=*/nullptr
    );

    if (!m_peerConnectionFactory) {
        std::cerr << "Error creating PeerConnectionFactory" << std::endl;
        return false;
    }
    return true;
}
