//
// Created by donggu on 2026/9/10.
//

#include "WebrtcManager.h"

#include <iostream>

#include <QDebug>
#include <QVideoFrame>

#include "api/audio_options.h"
#include "api/make_ref_counted.h"
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

    // 预创建音轨
    if (getOrCreateAudioTrack() == nullptr) {
        std::cerr << "[WebrtcManager] 音轨预创建失败,本次运行将没有语音功能" << std::endl;
    }

    // 预创建视频轨
    if (getOrCreateVideoTrack() == nullptr) {
        std::cerr << "[WebrtcManager] 视频轨预创建失败,本次运行将没有画面共享" << std::endl;
    }

    // 创建共享模式下的接收端渲染器
    m_remoteRenderer = std::make_unique<RemoteVideoRenderer>();

    return true;
}

// ============================
// 对端管理
// ============================

bool WebrtcManager::createPeerConnection(const QString &peerId, bool createDataChannel) {
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

    // PeerConnection 配置。STUN 服务器列表由上层传入,TURN 服务器暂不支持。
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    for (const auto &stunServer : m_stunServers) {
        webrtc::PeerConnectionInterface::IceServer iceServer;
        iceServer.urls.push_back(stunServer);
        config.servers.push_back(iceServer);
    }

    // 创建新的 PeerLink 实例
    auto *link = new PeerLink(peerId, m_peerConnectionFactory, config,
                              createDataChannel, this);
    if (!link->isValid()) {
        delete link;
        return false;
    }

    // createOffer前挂载本地音视频轨，否则 SDP 里不会有 m=audio/m=video 段,对端就收不到音视频。
    if (m_localAudioTrack != nullptr)
        link->addLocalAudioTrack(m_localAudioTrack);

    if (m_localVideoTrack != nullptr)
        link->addLocalVideoTrack(m_localVideoTrack);

    // 设置共享模式下接收端解码后使用的sink
    if (m_remoteRenderer != nullptr)
        link->setRemoteVideoSink(m_remoteRenderer.get());


    m_peers.insert(peerId, link);
    std::cout << "[WebrtcManager] 已建立对端连接: " << peerId.toStdString()
              << "(当前共 " << m_peers.size() << " 条)" << std::endl;
    return true;
}

void WebrtcManager::removePeer(const QString &peerId) {
    PeerLink *link = m_peers.take(peerId);
    if (link == nullptr)
        return;

    // 先关闭连接再delete，防止析构过程中发生回调
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

    // 创建音频源，所有track共享同一个源
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

void WebrtcManager::pushVideoFrame(const webrtc::VideoFrame &frame) {
    if (m_videoSource == nullptr)
        return;

    m_videoSource->pushFrame(frame);
}

void WebrtcManager::pushQtVideoFrame(const QVideoFrame &frame) {
    static int n = 0;
    if (n < 10 || n%300 == 0) {
        qDebug() << "[诊断] pushQtVideoFrame 第" << (n + 1) << "帧,"
                 << "videoSource =" << (void *)m_videoSource.get();
    }
    ++n;

    if (m_videoSource == nullptr)
        return;

    m_videoSource->pushQtFrame(frame);
}

void WebrtcManager::setRemoteVideoSink(QVideoSink *sink) {
    if (m_remoteRenderer == nullptr)
        return;

    // 将播放器的sink传给渲染器,渲染器解码后通过这个sink把帧送给播放器显示
    m_remoteRenderer->setTargetSink(sink);
}

void WebrtcManager::setRemoteVideoEnabled(bool enabled) {
    if (m_remoteRenderer == nullptr)
        return;

    // 只有作为接收端时才往sink写帧,发送端不写避免回声覆盖本地画面
    m_remoteRenderer->setWritingEnabled(enabled);
}

webrtc::scoped_refptr<webrtc::VideoTrackInterface> WebrtcManager::getOrCreateVideoTrack() {
    if (m_localVideoTrack != nullptr)
        return m_localVideoTrack;

    if (m_peerConnectionFactory == nullptr || !m_initialized) {
        std::cerr << "[WebrtcManager] 尚未初始化,无法创建视频轨" << std::endl;
        return nullptr;
    }

    // 创建视频源，所有track共享同一个源
    m_videoSource = webrtc::scoped_refptr<VideoTrackSource>(new VideoTrackSource());

    // 视频轨是"把视频源接到 PeerConnection 上"的插头
    // 这里建的是原型,每个对端拿它去 AddTrack 时 WebRTC 会各建一条独立的流。
    m_localVideoTrack = m_peerConnectionFactory->CreateVideoTrack(m_videoSource,
                                                                  "syncine-video");
    if (m_localVideoTrack == nullptr) {
        std::cerr << "[WebrtcManager] 创建视频轨失败" << std::endl;
        return nullptr;
    }

    std::cout << "[WebrtcManager] 视频轨已创建" << std::endl;
    return m_localVideoTrack;
}

void WebrtcManager::destroy() {
    // 释放顺序是有依赖的,不能随意调整: 对端 → 音轨 → 音频源 → 工厂 → 线程
    // 理由:
    //   - 对端要在最前:关连接时 WebRTC 还可能在回调,线程必须还活着
    //   - 音轨/音频源要在工厂之前:它们内部持有工厂引用,
    //     工厂先没了它们就指向一块已释放的内存
    //   - 音轨要在音频源之前:音轨引用音频源,反过来则会留下悬空引用
    //   - 工厂要在线程之前:工厂内部引用这三根线程,
    //     先停线程会让工厂里的引用失效

    // 1. 对端连接 —— 必须在停线程之前
    closeAllPeers();

    // 2. 轨 → 源 → 工厂(顺序同上:引用方先释放)
    m_localVideoTrack = nullptr;
    m_localAudioTrack = nullptr;
    m_videoSource = nullptr;
    m_audioSource = nullptr;

    // 接收侧渲染器也要在工厂之前放掉:它内部的转换会用到 WebRTC 的类型
    m_remoteRenderer.reset();

    m_peerConnectionFactory = nullptr;

    // 3. 线程 —— 必须在工厂之后
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
