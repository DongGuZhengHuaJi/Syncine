//
// Created by donggu on 2026/9/10.
//

#include "WebrtcManager.h"

#include <QDebug>
#include <QVideoFrame>

#include "MovieAudioPlayer.h"
#include "core/Log.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/audio_decoder_factory.h"
#include "api/audio_codecs/audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/environment/environment_factory.h"
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

namespace {

bool startThreadSet(std::unique_ptr<webrtc::Thread> &network,
                    std::unique_ptr<webrtc::Thread> &worker,
                    std::unique_ptr<webrtc::Thread> &signaling,
                    const char *which) {
    network = webrtc::Thread::CreateWithSocketServer();
    signaling = webrtc::Thread::Create();
    worker = webrtc::Thread::Create();

    if (!network->Start() || !signaling->Start() || !worker->Start()) {
        LOG_ERROR("WebRTC") << which << "线程启动失败";
        // 线程启动失败，直接关闭
        network->Stop();
        signaling->Stop();
        worker->Stop();
        network.reset();
        signaling.reset();
        worker.reset();
        return false;
    }
    return true;
}

void stopThreadSet(std::unique_ptr<webrtc::Thread> &network,
                   std::unique_ptr<webrtc::Thread> &worker,
                   std::unique_ptr<webrtc::Thread> &signaling) {
    if (network != nullptr) {
        network->Stop();
        network.reset();
    }
    if (worker != nullptr) {
        worker->Stop();
        worker.reset();
    }
    if (signaling != nullptr) {
        signaling->Stop();
        signaling.reset();
    }
}

// ----------------------------------------------------------------------
// 立体声 Opus 编解码器工厂包装 —— media 工厂专用
//
// 【为什么需要这两个包装】
//
// SDP 里 Opus 的"2 声道"和"立体声"是两个概念:rtpmap 的 opus/48000/2
// 只是"最多 2 声道"的能力声明;真正决定编码器开几个声道的是 fmtp 里的
// stereo=1 参数(modules/audio_coding/codecs/opus/audio_encoder_opus.cc
// 的 GetChannelCount:参数不是 "1" 一律按 1 声道编码)。
//
// 而内置工厂通告的 Opus 格式**不带** stereo=1(同文件 AppendSupportedEncoders
// 只写了 minptime 和 useinbandfec)—— 所以用内置工厂协商出来的 Opus 永远
// 单声道:推立体声 PCM 进去也会被 ACM 按编码器声道数下混(ReMixFrame)。
//
// 解法:把 Opus 格式的参数补上 stereo=1,让它进 SDP fmtp(media/base/
// codec.cc 的 Codec(SdpAudioFormat) 拷贝全部参数)。两端都传,协商结果
// 就带 stereo=1,编码器按 2 声道工作(webrtc_voice_engine.cc 的
// UpdateSendCodecSpec:stereo=1 → num_encoded_channels_ = 2)。
//
// 【为什么编码器和解码器两个工厂都要包】
//
// 电影音轨的 m-line 是 sendrecv,它的 codec 列表是发送、接收两个列表合并
// 出来的(pc/codec_vendor.cc 的 audio_sendrecv_codecs)。合并用的是
// NegotiateCodecs(recv 列表, send 列表),而它产出的是
// `Codec negotiated = ours` —— **参数取的是 recv 列表那份**。
// 只包编码器工厂时,stereo=1 在 send 列表里,合并时被丢掉,协商结果仍是
// 单声道 —— 这是实测踩过的坑。所以解码器工厂必须一起包。
//
// 【为什么只给 media 工厂】
//
// voice 工厂继续用内置工厂:麦克风是单声道,给它协商立体声只会让编码器
// 白白上混、白花带宽。电影音频才需要立体声,而这两个包装只影响传给它的
// 那一套工厂 —— 两条连接、两种声道配置,互不干扰。
//
// 【顺带的音质参数】
//
// maxaveragebitrate:Opus 默认目标码率(约 32kbps)对音乐偏低。电影配乐
// 值得提到 64kbps,局域网共享带宽不是瓶颈。想再高可以调,范围 6~510kbps
// (见 audio_encoder_opus.cc 的 CalculateBitrate 钳制)。它同样由
// recv 列表的参数主导,所以两个包装里都写。
// ----------------------------------------------------------------------

class StereoOpusAudioEncoderFactory : public webrtc::AudioEncoderFactory {
public:
    StereoOpusAudioEncoderFactory()
        : m_inner(webrtc::CreateBuiltinAudioEncoderFactory()) {
    }

    std::vector<webrtc::AudioCodecSpec> GetSupportedEncoders() override {
        std::vector<webrtc::AudioCodecSpec> specs = m_inner->GetSupportedEncoders();
        for (webrtc::AudioCodecSpec &spec : specs) {
            if (spec.format.name == "opus") {
                spec.format.parameters["stereo"] = "1";
                spec.format.parameters["maxaveragebitrate"] = "64000";
            }
        }
        return specs;
    }

    std::optional<webrtc::AudioCodecInfo> QueryAudioEncoder(
        const webrtc::SdpAudioFormat &format) override {
        // 内置工厂本来就认得 stereo/maxaveragebitrate 参数,直接转发。
        return m_inner->QueryAudioEncoder(format);
    }

    std::unique_ptr<webrtc::AudioEncoder> Create(
        const webrtc::Environment &env,
        const webrtc::SdpAudioFormat &format,
        Options options) override {
        return m_inner->Create(env, format, options);
    }

private:
    const webrtc::scoped_refptr<webrtc::AudioEncoderFactory> m_inner;
};

// 解码器侧的同款包装。见上面"为什么两个工厂都要包"。
class StereoOpusAudioDecoderFactory : public webrtc::AudioDecoderFactory {
public:
    StereoOpusAudioDecoderFactory()
        : m_inner(webrtc::CreateBuiltinAudioDecoderFactory()) {
    }

    std::vector<webrtc::AudioCodecSpec> GetSupportedDecoders() override {
        std::vector<webrtc::AudioCodecSpec> specs = m_inner->GetSupportedDecoders();
        for (webrtc::AudioCodecSpec &spec : specs) {
            if (spec.format.name == "opus") {
                spec.format.parameters["stereo"] = "1";
                spec.format.parameters["maxaveragebitrate"] = "64000";
            }
        }
        return specs;
    }

    bool IsSupportedDecoder(const webrtc::SdpAudioFormat &format) override {
        return m_inner->IsSupportedDecoder(format);
    }

    std::unique_ptr<webrtc::AudioDecoder> Create(
        const webrtc::Environment &env,
        const webrtc::SdpAudioFormat &format,
        std::optional<webrtc::AudioCodecPairId> codec_pair_id) override {
        return m_inner->Create(env, format, codec_pair_id);
    }

private:
    const webrtc::scoped_refptr<webrtc::AudioDecoderFactory> m_inner;
};

} // namespace

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

    // 在 media 工厂初始化前创建 dummyAdm和 Environment
    m_mediaEnvironment.emplace(webrtc::CreateEnvironment());
    m_dummyAdm = webrtc::CreateAudioDeviceModule(*m_mediaEnvironment, webrtc::AudioDeviceModule::kDummyAudio);
    if (m_dummyAdm == nullptr) {
        LOG_ERROR("WebRTC") << "创建假声卡失败,放弃初始化";
        m_mediaEnvironment.reset();
        return false;
    }

    if (!initializeVoiceFactory()) {
        LOG_ERROR("WebRTC") << "初始化 voice 工厂失败,放弃初始化";
        return false;
    }

    if (!initializeMediaFactory()) {
        LOG_ERROR("WebRTC") << "初始化 media 工厂失败,放弃初始化";
        m_dummyAdm = nullptr;
        m_mediaEnvironment.reset();
        destroy();
        return false;
    }

    m_stunServers = stunServers;
    m_initialized = true;
    LOG_INFO("WebRTC") << "WebRTC 初始化成功";

    // 预创建各条轨
    if (getOrCreateAudioTrack() == nullptr) {
        LOG_WARN("WebRTC") << "推流语音轨预创建失败,本次运行将无法共享麦克风声音";
    }
    if (getOrCreateVideoTrack() == nullptr) {
        LOG_WARN("WebRTC") << "推流视频轨预创建失败,本次运行将无法共享视频画面";
    }
    if (getOrCreateMovieAudioTrack() == nullptr) {
        LOG_WARN("WebRTC") << "推流音频轨预创建失败,本次运行将没有共享视频声音";
    }

    // 预创建共享模式下接收侧的画面渲染器和声音播放器
    // todo: 实现音画同步
    m_remoteRenderer = std::make_unique<RemoteVideoRenderer>();
    m_movieAudioPlayer = std::make_unique<MovieAudioPlayer>();
    m_movieAudioPlayer->setVolume(m_remoteMovieVolume);

    return true;
}

// ============================
// 对端管理
// ============================

PeerLink *WebrtcManager::createLink(const QString &peerId, LinkKind kind,
                                    bool createDataChannel) {
    const bool isVoice = (kind == LinkKind::Voice);
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory =
        isVoice ? m_voiceFactory : m_mediaFactory;

    if (factory == nullptr) {
        LOG_ERROR("WebRTC") << (isVoice ? "voice" : "media") << "工厂为空,无法建立连接";
        return nullptr;
    }

    webrtc::PeerConnectionInterface::RTCConfiguration config;
    for (const auto &stunServer : m_stunServers) {
        webrtc::PeerConnectionInterface::IceServer iceServer;
        iceServer.urls.push_back(stunServer);
        config.servers.push_back(iceServer);
    }

    auto *link = new PeerLink(peerId, kind, factory, config, createDataChannel, this);
    if (!link->isValid()) {
        LOG_ERROR("WebRTC") << (isVoice ? "voice" : "media") << "连接创建失败,已放弃";
        delete link;
        return nullptr;
    }

    // 在 createOffer 之前挂上本地轨,SDP 中才会包含相应的 m=audio/m=video 段。
    // todo: 完善重新协商逻辑
    if (isVoice) {
        if (m_localAudioTrack != nullptr)
            link->addLocalAudioTrack(m_localAudioTrack);
        link->setRemoteAudioVolume(m_remoteChatVolume);
    } else {
        if (m_localVideoTrack != nullptr)
            link->addLocalVideoTrack(m_localVideoTrack);
        if (m_localMovieAudioTrack != nullptr)
            link->addLocalMovieAudioTrack(m_localMovieAudioTrack);

        // 接收侧:远端画面进共享渲染器
        if (m_remoteRenderer != nullptr)
            link->setRemoteVideoSink(m_remoteRenderer.get());

        // 接收侧:远端电影声进我们自己的播放器
        if (m_movieAudioPlayer != nullptr)
            link->setRemoteAudioSink(m_movieAudioPlayer.get());

        // 显示关闭 media 连接的播放,让 NullAudioPoller 接管,每 10ms 自动拉取音频数据进入sink
        link->setPlayoutEnabled(false);
    }

    return link;
}

bool WebrtcManager::createPeerConnection(const QString &peerId, bool createDataChannel) {
    if (!m_initialized) {
        LOG_ERROR("WebRTC") << "尚未初始化,无法建立对端连接";
        return false;
    }

    if (peerId.isEmpty()) {
        LOG_WARN("WebRTC") << "对端 ID 为空,无法建立连接";
        return false;
    }

    // 幂等:已存在就直接返回 true,不再重复建两条连接
    if (m_peers.contains(peerId))
        return true;

    PeerLinks links;
    links.voice = createLink(peerId, LinkKind::Voice, createDataChannel);
    links.media = createLink(peerId, LinkKind::Media, /*createDataChannel=*/false);

    if (links.voice == nullptr || links.media == nullptr) {
        LOG_ERROR("WebRTC") << "对端" << peerId << "的连接建立失败(voice:"
                            << (links.voice ? "ok" : "fail")
                            << "media:" << (links.media ? "ok" : "fail") << ")";
        if (links.voice != nullptr) {
            links.voice->close();
            links.voice->deleteLater();
        }
        if (links.media != nullptr) {
            links.media->close();
            links.media->deleteLater();
        }
        return false;
    }

    m_peers.insert(peerId, links);
    LOG_INFO("WebRTC") << "已建立对端连接:" << peerId << "(两条:voice + media,当前共"
                       << m_peers.size() << "个对端)";
    return true;
}

void WebrtcManager::removePeer(const QString &peerId) {
    PeerLinks links = m_peers.take(peerId);

    // 先关闭连接再 deleteLater，防止析构过程中发生回调
    if (links.voice != nullptr) {
        links.voice->close();
        links.voice->deleteLater();
    }
    if (links.media != nullptr) {
        links.media->close();
        links.media->deleteLater();
    }

    if (links.voice != nullptr || links.media != nullptr) {
        LOG_INFO("WebRTC") << "已移除对端连接:" << peerId << "(剩余" << m_peers.size()
                           << "个对端)";
    }
}

void WebrtcManager::closeAllPeers() {
    const QList<QString> ids = m_peers.keys();
    for (const QString &id : ids)
        removePeer(id);
}

PeerLink *WebrtcManager::peer(const QString &peerId, LinkKind kind) const {
    const auto it = m_peers.constFind(peerId);
    if (it == m_peers.constEnd())
        return nullptr;
    return kind == LinkKind::Voice ? it->voice : it->media;
}

// ============================
// 麦克风
// ============================

webrtc::scoped_refptr<webrtc::AudioTrackInterface> WebrtcManager::getOrCreateAudioTrack() {
    if (m_localAudioTrack != nullptr)
        return m_localAudioTrack;

    if (m_voiceFactory == nullptr || !m_initialized) {
        LOG_ERROR("WebRTC") << "尚未初始化或 voice 工厂未创建,无法创建音轨";
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

        m_audioSource = m_voiceFactory->CreateAudioSource(options);
        if (m_audioSource == nullptr) {
            LOG_ERROR("WebRTC") << "创建音频源失败";
            return nullptr;
        }
    }

    m_localAudioTrack = m_voiceFactory->CreateAudioTrack("syncine-mic", m_audioSource.get());
    if (m_localAudioTrack == nullptr) {
        LOG_ERROR("WebRTC") << "创建音轨失败";
        return nullptr;
    }

    m_localAudioTrack->set_enabled(false);

    LOG_INFO("WebRTC") << "推流语音轨已创建";
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

    LOG_INFO("WebRTC") << "麦克风已" << (enabled ? "开启" : "关闭") << "(当前"
                       << m_peers.size() << "个对端)";
    emit audioEnabledChanged(enabled);
}

// ============================
// 电影音频
// ============================

bool WebrtcManager::movieAudioEnabled() const {
    return m_localMovieAudioTrack != nullptr && m_localMovieAudioTrack->enabled();
}

void WebrtcManager::setMovieAudioEnabled(bool enabled) {
    if (!getOrCreateMovieAudioTrack()) {
        emit movieAudioEnabledChanged(false);
        return;
    }

    if (m_localMovieAudioTrack->enabled() == enabled)
        return;

    m_localMovieAudioTrack->set_enabled(enabled);

    // 这条跟着播放/暂停走,频率不低,放 Debug
    LOG_DEBUG("WebRTC") << "电影音轨已" << (enabled ? "开启" : "关闭") << "(当前"
                        << m_peers.size() << "个对端)";
    emit movieAudioEnabledChanged(enabled);
}

void WebrtcManager::pushMovieAudioPcm(const int16_t *data, size_t samplesPerChannel,
                                      int sampleRate, size_t channels) {
    if (m_movieAudioSource == nullptr) {
        LOG_WARN("WebRTC") << "接收侧音频播放器未创建,无法推送 PCM";
        return;
    }

    m_movieAudioSource->pushPcm(data, samplesPerChannel, sampleRate, channels);
}

webrtc::scoped_refptr<webrtc::AudioTrackInterface> WebrtcManager::getOrCreateMovieAudioTrack() {
    if (m_localMovieAudioTrack != nullptr)
        return m_localMovieAudioTrack;

    if (m_mediaFactory == nullptr || !m_initialized) {
        LOG_ERROR("WebRTC") << "尚未初始化或 media 工厂未创建,无法创建音轨";
        return nullptr;
    }

    // 实现了 AudioSourceInterface 的 MovieAudioSource,它把 PCM 推给 WebRTC
    if (m_movieAudioSource == nullptr) {
        m_movieAudioSource = webrtc::make_ref_counted<MovieAudioSource>();
    }

    m_localMovieAudioTrack = m_mediaFactory->CreateAudioTrack(
        "syncine-movie", m_movieAudioSource.get());
    if (m_localMovieAudioTrack == nullptr) {
        LOG_ERROR("WebRTC") << "创建电影音轨失败";
        return nullptr;
    }

    m_localMovieAudioTrack->set_enabled(false);

    LOG_INFO("WebRTC") << "推流音频轨已创建";
    return m_localMovieAudioTrack;
}

// ============================
// 接收端音量
// ============================

void WebrtcManager::setRemoteMovieVolume(double volume) {
    volume = qBound(0.0, volume, 1.0);

    // 用差值判断是否相等
    if (qFuzzyIsNull(m_remoteMovieVolume - volume))
        return;

    m_remoteMovieVolume = volume;

    // 电影声与 WebRTC 无关,由 MovieAudioPlayer 控制
    if (m_movieAudioPlayer != nullptr)
        m_movieAudioPlayer->setVolume(volume);

    emit remoteMovieVolumeChanged(volume);
}

void WebrtcManager::setRemoteChatVolume(double volume) {
    volume = qBound(0.0, volume, 1.0);

    if (qFuzzyIsNull(m_remoteChatVolume - volume))
        return;

    m_remoteChatVolume = volume;

    // 语音音量由 WebRTC 自己的播放控制,所以要遍历所有对端连接,把 volume 传给它们的 PeerLink
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        if (it->voice != nullptr)
            it->voice->setRemoteAudioVolume(volume);
    }

    emit remoteChatVolumeChanged(volume);
}

// ============================
// 视频
// ============================

void WebrtcManager::pushVideoFrame(const webrtc::VideoFrame &frame) {
    if (m_videoSource == nullptr)
        return;

    m_videoSource->pushFrame(frame);
}

void WebrtcManager::pushQtVideoFrame(const QVideoFrame &frame) {
    static int n = 0;
    if (n < 3 || n % 300 == 0) {
        LOG_TRACE("WebRTC") << "pushQtVideoFrame 第" << (n + 1) << "帧, videoSource ="
                            << (void *) m_videoSource.get();
    }
    ++n;

    if (m_videoSource == nullptr)
        return;

    m_videoSource->pushQtFrame(frame);
}

void WebrtcManager::setRemoteVideoSink(QVideoSink *sink) {
    if (m_remoteRenderer == nullptr)
        return;

    m_remoteRenderer->setTargetSink(sink);
}

void WebrtcManager::setRemoteVideoEnabled(bool enabled) {
    if (m_remoteRenderer == nullptr)
        return;

    m_remoteRenderer->setWritingEnabled(enabled);
}

webrtc::scoped_refptr<webrtc::VideoTrackInterface> WebrtcManager::getOrCreateVideoTrack() {
    if (m_localVideoTrack != nullptr)
        return m_localVideoTrack;

    if (m_mediaFactory == nullptr || !m_initialized) {
        LOG_ERROR("WebRTC") << "尚未初始化或 media 工厂未创建,无法创建视频轨";
        return nullptr;
    }

    m_videoSource = webrtc::scoped_refptr<VideoTrackSource>(new VideoTrackSource());

    m_localVideoTrack = m_mediaFactory->CreateVideoTrack(m_videoSource, "syncine-video");
    if (m_localVideoTrack == nullptr) {
        LOG_ERROR("WebRTC") << "创建视频轨失败";
        return nullptr;
    }

    LOG_INFO("WebRTC") << "推流视频轨已创建";
    return m_localVideoTrack;
}

// ============================
// 工厂与线程
// ============================

bool WebrtcManager::initializeVoiceFactory() {
    if (!startThreadSet(m_voiceNetworkThread, m_voiceWorkerThread,
                        m_voiceSignalingThread, "voice")) {
        return false;
    }

    // 真声卡:default_adm 传 nullptr,让 WebRTC 用平台默认设备。
    // 回声消除、自动增益、噪声抑制都靠它,语音质量全在这里。
    m_voiceFactory = webrtc::CreatePeerConnectionFactory(
        m_voiceNetworkThread.get(),
        m_voiceWorkerThread.get(),
        m_voiceSignalingThread.get(),
        /*default_adm=*/nullptr,
        webrtc::CreateBuiltinAudioEncoderFactory(),
        webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(),
        /*audio_mixer=*/nullptr,
        /*audio_processing=*/nullptr
    );

    if (m_voiceFactory == nullptr) {
        LOG_ERROR("WebRTC") << "创建 voice 工厂失败";
        stopThreadSet(m_voiceNetworkThread, m_voiceWorkerThread, m_voiceSignalingThread);
        return false;
    }
    return true;
}

bool WebrtcManager::initializeMediaFactory() {
    if (!startThreadSet(m_mediaNetworkThread, m_mediaWorkerThread,
                        m_mediaSignalingThread, "media")) {
        return false;
    }

    m_mediaFactory = webrtc::CreatePeerConnectionFactory(
        m_mediaNetworkThread.get(),
        m_mediaWorkerThread.get(),
        m_mediaSignalingThread.get(),
        m_dummyAdm,
        webrtc::make_ref_counted<StereoOpusAudioEncoderFactory>(),
        webrtc::make_ref_counted<StereoOpusAudioDecoderFactory>(),
        webrtc::CreateBuiltinVideoEncoderFactory(),
        webrtc::CreateBuiltinVideoDecoderFactory(),
        /*audio_mixer=*/nullptr,
        /*audio_processing=*/nullptr
    );

    if (m_mediaFactory == nullptr) {
        LOG_ERROR("WebRTC") << "创建 media 工厂失败";
        stopThreadSet(m_mediaNetworkThread, m_mediaWorkerThread, m_mediaSignalingThread);
        return false;
    }
    return true;
}

void WebrtcManager::destroy() {
    // 释放顺序是有依赖的,不能随意调整: 对端 → 轨/源 → 工厂 → 线程
    // 理由:
    //   - 对端要在最前:关连接时 WebRTC 还可能在回调,线程必须还活着
    //   - 轨/音频源要在工厂之前:它们内部持有工厂引用,
    //     工厂先没了它们就指向一块已释放的内存
    //   - 轨要在源之前:轨引用源,反过来则会留下悬空引用
    //   - 工厂要在线程之前:工厂内部引用这些线程,
    //     先停线程会让工厂里的引用失效

    // 1. 对端连接 —— 必须在停线程之前
    closeAllPeers();

    // 接收侧播放器要在工厂之前放掉:它内部持有 WebRTC 的 sink 注册关系
    m_movieAudioPlayer.reset();

    // 2. 轨 → 源 → 工厂(顺序同上:引用方先释放)
    m_localVideoTrack = nullptr;
    m_localAudioTrack = nullptr;
    m_localMovieAudioTrack = nullptr;
    m_videoSource = nullptr;
    m_audioSource = nullptr;
    m_movieAudioSource = nullptr;

    // 接收侧渲染器也要在工厂之前放掉:它内部的转换会用到 WebRTC 的类型
    m_remoteRenderer.reset();

    m_voiceFactory = nullptr;
    m_mediaFactory = nullptr;

    // 3. 线程 —— 必须在工厂之后
    stopThreadSet(m_voiceNetworkThread, m_voiceWorkerThread, m_voiceSignalingThread);
    stopThreadSet(m_mediaNetworkThread, m_mediaWorkerThread, m_mediaSignalingThread);

    // 4. 假声卡和环境放最后:工厂已经不再持有它们了
    m_dummyAdm = nullptr;
    m_mediaEnvironment.reset();

    m_initialized = false;
}
