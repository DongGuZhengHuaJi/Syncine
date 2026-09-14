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

// ============================
// 初始化
// ============================

bool WebrtcManager::initialize(const std::vector<std::string> &stunServers) {
    if (m_initialized)
        return true;

    if (!initializeThreads())
        return false;

    if (!initializeFactory()) {
        destroy();
        return false;
    }

    m_stunServers = stunServers;
    m_initialized = true;
    return true;
}

// ============================
// 对端管理
// ============================

bool WebrtcManager::createPeerConnection(const QString &peerId, bool createDataChannel) {
    // 这两条是管理器自身的内部校验,不是连接级错误 —— 所以走日志而不是信号。
    // 连接级错误(ICE 失败、协商失败)由 PeerLink 发出,那条路是给上层看的。
    if (!m_initialized) {
        std::cerr << "[WebrtcManager] 尚未初始化,无法建立对端连接" << std::endl;
        return false;
    }

    if (peerId.isEmpty()) {
        std::cerr << "[WebrtcManager] 对端 ID 为空,无法建立连接" << std::endl;
        return false;
    }

    // 幂等:已经建过就直接返回。
    if (m_peers.contains(peerId))
        return true;

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    for (const auto &stunServer : m_stunServers) {
        webrtc::PeerConnectionInterface::IceServer iceServer;
        iceServer.urls.push_back(stunServer);
        config.servers.push_back(iceServer);
    }

    auto *link = new PeerLink(peerId, m_peerConnectionFactory, config,
                              createDataChannel, this);
    if (!link->isValid()) {
        delete link;
        return false;
    }

    if (m_localAudioTrack != nullptr)
        link->addLocalAudioTrack(m_localAudioTrack);

    // 这里不接任何信号。PeerLink 的信号由 SignalingChannel 直接连 ——
    // 它才是这些消息的消费者,管理器不参与转发。
    m_peers.insert(peerId, link);
    std::cout << "[WebrtcManager] 已建立对端连接: " << peerId.toStdString()
              << "(当前共 " << m_peers.size() << " 条)" << std::endl;
    return true;
}

void WebrtcManager::removePeer(const QString &peerId) {
    PeerLink *link = m_peers.take(peerId);
    if (link == nullptr)
        return;

    // 先关连接,再 delete。
    // 顺序反了的话,delete 期间 WebRTC 信令线程上可能还有回调在跑,
    // 虚表里指向的就是一块已释放的内存。
    link->close();
    link->deleteLater();

    std::cout << "[WebrtcManager] 已移除对端连接: " << peerId.toStdString()
              << "(剩余 " << m_peers.size() << " 条)" << std::endl;
}

void WebrtcManager::closeAllPeers() {
    const QList<QString> ids = m_peers.keys();
    for (const QString &id : ids)
        removePeer(id);
}

PeerLink *WebrtcManager::peer(const QString &peerId) const {
    return m_peers.value(peerId, nullptr);
}

void WebrtcManager::setLocalAudioTrack(webrtc::scoped_refptr<webrtc::AudioTrackInterface> track) {
    m_localAudioTrack = std::move(track);

    if (m_localAudioTrack == nullptr)
        return;

    for (PeerLink *link : m_peers)
        link->addLocalAudioTrack(m_localAudioTrack);
}

void WebrtcManager::destroy() {
    // 必须在停线程之前:关连接的过程中还可能触发回调,需要线程还活着
    closeAllPeers();

    if (m_peerConnectionFactory != nullptr)
        m_peerConnectionFactory = nullptr;

    // 工厂释放后再停线程(工厂内部引用这些线程),否则析构运行中的线程会崩溃
    if (m_networkThread != nullptr) {
        m_networkThread->Stop();
        m_networkThread.reset();
    }
    if (m_workerThread != nullptr) {
        m_workerThread->Stop();
        m_workerThread.reset();
    }
    if (m_signalingThread != nullptr) {
        m_signalingThread->Stop();
        m_signalingThread.reset();
    }

    m_localAudioTrack = nullptr;
    m_initialized = false;
}

// ============================
// 线程与工厂
// ============================

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

    if (m_peerConnectionFactory == nullptr) {
        std::cerr << "Error creating PeerConnectionFactory" << std::endl;
        return false;
    }
    return true;
}
