//
// Created by donggu on 2026/9/10.
//

#include "WebrtcManager.h"

#include <iostream>

#include "api/audio_options.h"
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

    // 立刻把音轨准备好(保持禁用,不碰麦克风)。
    //
    // 为什么必须在这里,而不能等用户点"开启语音":
    //
    //   SDP 里的 m=audio 段只在**协商那一刻**决定。如果音轨是在
    //   协商完成之后才 AddTrack 的,WebRTC 会要求重新协商
    //   (renegotiation)才能把音频写进 SDP —— 而我们没有实现重协商,
    //   于是对端从头到尾都不知道要收音频,表现就是"点了没声音"。
    //
    //   音轨先建好并保持禁用,协商就能一次到位;之后开麦关麦只改
    //   track->set_enabled(),完全不碰 SDP,对端毫无感知。
    if (getOrCreateAudioTrack() == nullptr) {
        // 麦克风不可用(设备被占用、没有声卡)不该拖垮整个 WebRTC ——
        // 连接本身还有数据通道,后面还要承载视频。
        std::cerr << "[WebrtcManager] 音轨预创建失败,本次运行将没有语音功能" << std::endl;
    }

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

    // AddTrack后再createOffer
    if (m_localAudioTrack != nullptr)
        link->addLocalAudioTrack(m_localAudioTrack);

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

    if (m_localAudioTrack == nullptr) {
        emit audioEnabledChanged(false);
        return;
    }

    for (PeerLink *link : m_peers)
        link->addLocalAudioTrack(m_localAudioTrack);

    emit audioEnabledChanged(m_localAudioTrack->enabled());
}

// ============================
// 麦克风
// ============================

webrtc::scoped_refptr<webrtc::AudioTrackInterface> WebrtcManager::getOrCreateAudioTrack() {
    if (m_localAudioTrack != nullptr)
        return m_localAudioTrack;

    if (m_peerConnectionFactory == nullptr || !m_initialized) {
        std::cerr << "[WebrtcManager] 尚未初始化,无法创建音频源" << std::endl;
        return nullptr;
    }

    // 音频源描述"从哪取音"。全进程一个 —— 所有对端共用同一个麦克风。
    //
    // AudioOptions 这里设定的不是"建议",而是**全局生效**的音频处理配置
    // (见 api/peer_connection_interface.h 中 CreateAudioSource 的注释):
    // 它会下发到媒体引擎,决定整条链路开不开 AEC/AGC/NS。
    if (m_audioSource == nullptr) {
        webrtc::AudioOptions options;
        // 扬声器外放 + 麦克风同时工作的场景下,不消回声就是啸叫
        options.echo_cancellation = true;
        // 不同人说话的远近、音量差很大,自动增益能把它们拉平
        options.auto_gain_control = true;
        // 风扇、键盘、空调底噪
        options.noise_suppression = true;
        // 过滤低频轰鸣(桌面震动、空调)
        options.highpass_filter = true;

        m_audioSource = m_peerConnectionFactory->CreateAudioSource(options);
        if (m_audioSource == nullptr) {
            std::cerr << "[WebrtcManager] 创建音频源失败(麦克风可能不可用)" << std::endl;
            return nullptr;
        }
    }

    // 音轨是"把音频源接到 PeerConnection 上"的插头。
    // 这里建的是原型,每个对端拿它去 AddTrack 时 WebRTC 会各建一条独立的流。
    m_localAudioTrack = m_peerConnectionFactory->CreateAudioTrack("syncine-mic", m_audioSource.get());
    if (m_localAudioTrack == nullptr) {
        std::cerr << "[WebrtcManager] 创建音轨失败" << std::endl;
        return nullptr;
    }

    m_localAudioTrack->set_enabled(false);

    return m_localAudioTrack;
}

void WebrtcManager::setAudioEnabled(bool enabled) {
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> track = getOrCreateAudioTrack();
    if (track == nullptr) {
        emit audioEnabledChanged(false);
        return;
    }

    if (track->enabled() == enabled)
        return;

    track->set_enabled(enabled);

    std::cout << "[WebrtcManager] 麦克风已" << (enabled ? "开启" : "关闭")
              << "(当前 " << m_peers.size() << " 条对端连接)" << std::endl;
    emit audioEnabledChanged(enabled);
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

    // 音轨和音频源都要在工厂之前释放 —— 它们内部引用工厂，
    // 工厂先没了会留下悬空引用。
    m_localAudioTrack = nullptr;
    m_audioSource = nullptr;
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
