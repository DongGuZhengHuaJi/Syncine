//
// Created by donggu on 2026/9/10.
//

#include "WebrtcManager.h"

#include <QDebug>
#include <QVideoFrame>

#include "MovieAudioPlayer.h"
#include "core/AudioDevices.h"
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

// 服务端**什么都没给**时的兜底(老服务端、或者它的字段没解析出来)。
// 一个公共 STUN:不知道自己的公网映射地址时,STUN 至少能问出来,
// 局域网和普通 NAT 照样直连 —— 少的只是对称 NAT 下的中继。
//
// **STUN 的 URL 不能带 query**。libwebrtc 的 ICE URL 解析明确拒绝
// "stun:host:port?transport=tcp" 这种写法,而且那是硬错误:任何一个 URL
// 不合法,**整条 PeerConnection 都建不起来**(不是跳过这一项),
// 症状只是 PeerLink 里一句"连接创建失败",极难往这上面想。
const QList<IceServerConfig> &defaultIceServers() {
    static const QList<IceServerConfig> servers = [] {
        IceServerConfig stun;
        stun.urls = {QStringLiteral("stun:stun.cloudflare.com:3478")};
        return QList<IceServerConfig>{stun};
    }();
    return servers;
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

// 画质档位 → 编码参数。
//
// 只调两个旋钮:**码率上限**和**帧率上限**,分辨率不动 ——
// 让编码器按码率自己降(过载时它本来就会先降分辨率),
// 比我们猜一个缩放系数更准。
//
// 关于"原画":WebRTC 里**不设上限不等于无限**。没设时它会用自己按分辨率算的
// 默认上限(video/config/encoder_stream_factory.cc 的 GetMaxDefaultVideoBitrateKbps:
// 540p 以上一律 2500kbps)——那比"高清"还低,所以"原画"必须显式给一个高上限,
// 真正的天花板交给带宽估计决定。
struct VideoQualityPreset {
    int maxBitrateBps = 0;   // 0 = 用 WebRTC 默认上限
    double maxFramerate = 0; // 0 = 不限制
};

VideoQualityPreset presetForQuality(WebrtcManager::VideoQuality quality) {
    switch (quality) {
    case WebrtcManager::VideoQuality::Smooth:
        // 流畅:320~480p 观感,帧率留 24(电影本身也就 24)
        return {800'000, 24.0};
    case WebrtcManager::VideoQuality::Standard:
        // 标准:720p 观感
        return {2'000'000, 30.0};
    case WebrtcManager::VideoQuality::Hd:
        // 高清:1080p 观感。已经高于 WebRTC 的默认天花板(2.5Mbps)
        return {4'000'000, 30.0};
    case WebrtcManager::VideoQuality::Original:
        // 原画:上限抬到很高,由网络决定能跑多少;帧率不限制
        return {10'000'000, 0.0};
    }
    return {10'000'000, 0.0};
}

QString qualityName(WebrtcManager::VideoQuality quality) {
    switch (quality) {
    case WebrtcManager::VideoQuality::Smooth: return QStringLiteral("流畅");
    case WebrtcManager::VideoQuality::Standard: return QStringLiteral("标准");
    case WebrtcManager::VideoQuality::Hd: return QStringLiteral("高清");
    case WebrtcManager::VideoQuality::Original: return QStringLiteral("原画");
    }
    return {};
}

// ---- 音频设备的三个小工具 ----

// 按名字在设备表里找索引。空名字(= 系统默认)或找不到都返回 0。
//
// 找不到时退回 0 而不是报错:设备被拔掉是常态(上次用的 USB 耳机这次没插),
// 退回"跟随系统默认"总比一个都不出声强。
int indexOfDeviceName(const QString &name,
                      const std::vector<WebrtcManager::DeviceInfo> &devices) {
    if (name.isEmpty())
        return 0;

    for (const WebrtcManager::DeviceInfo &device : devices) {
        // 0 号是 "default: xxx" 伪设备 —— 它由空名字表示,不参与按名字匹配
        if (device.index > 0 && device.name == name)
            return device.index;
    }
    return 0;
}

// 设备表 → OptionList 要的 [{label, value}]。
QVariantList devicesToVariant(const std::vector<WebrtcManager::DeviceInfo> &devices) {
    QVariantList list;
    for (const WebrtcManager::DeviceInfo &device : devices) {
        QVariantMap item;
        item[QStringLiteral("label")] = device.name;
        // 0 号在界面上就是"跟随系统默认",值用**空串**表示:它在 ADM 里的名字
        // 是 "default: <当前默认设备>",会随系统默认设备变化,存下来就会失效。
        item[QStringLiteral("value")] = device.index == 0 ? QString() : device.name;
        list.append(item);
    }
    return list;
}

} // namespace

WebrtcManager::WebrtcManager() {
}

WebrtcManager::~WebrtcManager() {
    destroy();
}

// ============================
// 初始化
// ============================

bool WebrtcManager::initialize() {
    if (m_initialized)
        return true;

    // 在voice和media 工厂初始化前创建环境和adm
    m_mediaEnvironment.emplace(webrtc::CreateEnvironment());

    m_voiceAdm = webrtc::CreateAudioDeviceModule(*m_mediaEnvironment, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
    if (m_voiceAdm == nullptr) {
        LOG_ERROR("WebRTC") << "创建 voice 声卡失败,放弃初始化";
        m_mediaEnvironment.reset();
        return false;
    }
    m_dummyAdm = webrtc::CreateAudioDeviceModule(*m_mediaEnvironment, webrtc::AudioDeviceModule::kDummyAudio);
    if (m_dummyAdm == nullptr) {
        LOG_ERROR("WebRTC") << "创建 media 声卡失败,放弃初始化";
        m_mediaEnvironment.reset();
        return false;
    }

    if (!initializeVoiceFactory()) {
        LOG_ERROR("WebRTC") << "初始化 voice 工厂失败,放弃初始化";
        // 两个 ADM 都引用了 Environment,必须比它先释放
        m_voiceAdm = nullptr;
        m_dummyAdm = nullptr;
        m_mediaEnvironment.reset();
        return false;
    }

    if (!initializeMediaFactory()) {
        LOG_ERROR("WebRTC") << "初始化 media 工厂失败,放弃初始化";
        m_dummyAdm = nullptr;
        m_mediaEnvironment.reset();
        destroy();
        return false;
    }

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
// ICE 配置
// ============================

void WebrtcManager::setIceServers(const QList<IceServerConfig> &servers) {
    // 成员一变就会走一遍"建连前刷配置",内容没变时不重复刷日志
    if (servers == m_iceServers)
        return;
    m_iceServers = servers;

    if (servers.isEmpty()) {
        LOG_WARN("WebRTC") << "服务端没有下发 ICE 配置,退回兜底 STUN;"
                              "对称 NAT 后面的对端可能连不上";
        return;
    }

    int urlCount = 0;
    bool hasTurn = false;
    for (const IceServerConfig &server : servers) {
        urlCount += server.urls.size();
        for (const QString &url : server.urls)
            hasTurn = hasTurn || url.startsWith(QLatin1String("turn"));
    }

    // 只记条数:username/credential 是临时密钥,任何级别都不写进日志
    LOG_INFO("WebRTC") << "ICE 配置已更新:" << servers.size() << "组服务器 /"
                       << urlCount << "个 URL" << (hasTurn ? "(含 TURN)" : "(仅 STUN)");
}

std::vector<webrtc::PeerConnectionInterface::IceServer> WebrtcManager::buildIceServers() const {
    const QList<IceServerConfig> &source =
        m_iceServers.isEmpty() ? defaultIceServers() : m_iceServers;

    std::vector<webrtc::PeerConnectionInterface::IceServer> servers;
    servers.reserve(source.size());

    for (const IceServerConfig &entry : source) {
        if (entry.urls.isEmpty())
            continue;

        webrtc::PeerConnectionInterface::IceServer server;
        for (const QString &url : entry.urls)
            server.urls.push_back(url.toStdString());

        server.username = entry.username.toStdString();
        // 协议里叫 credential(Cloudflare 的字段名),libwebrtc 的 IceServer
        // 里叫 password —— 同一件事,全项目只在这里翻译一次。
        server.password = entry.credential.toStdString();

        servers.push_back(std::move(server));
    }

    return servers;
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

    // ICE 配置(STUN + 可能带临时凭据的 TURN)在进房时由 SignalingChannel 交过来,
    // 见 setIceServers。配置是在下面 CreatePeerConnection 那一刻生效的 ——
    // 之后改 m_iceServers 不会影响这条连接。
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.servers = buildIceServers();

    // 诊断开关:SYNCINE_ICE_RELAY_ONLY=1 时只收集中继(relay)候选。
    // 用来回答"到底走没走 TURN" —— 这时只有 TURN 真的可用才连得上,
    // 能直连是说明不了问题的(局域网里本来就连得上)。默认关,别常开。
    if (qEnvironmentVariableIsSet("SYNCINE_ICE_RELAY_ONLY"))
        config.type = webrtc::PeerConnectionInterface::kRelay;

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
        if (m_localVideoTrack != nullptr) {
            link->addLocalVideoTrack(m_localVideoTrack);
            // 新连接套上当前画质档位(观众可能是在房主改过档位之后才进来的)
            const VideoQualityPreset preset = presetForQuality(m_videoQuality);
            link->applyVideoQuality(preset.maxBitrateBps, preset.maxFramerate);
        }
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

    // 插入之前先记:这是不是本次进房的第一条连接(引擎就是在这个时候初始化的)
    const bool firstPeer = m_peers.isEmpty();
    m_peers.insert(peerId, links);
    LOG_INFO("WebRTC") << "已建立对端连接:" << peerId << "(两条:voice + media,当前共"
                       << m_peers.size() << "个对端)";

    if (firstPeer) {
        // 上一步 CreatePeerConnection 内部会先把引擎拉起来,而
        // adm_helpers::Init() 会把声卡设备复位成"系统默认"。
        // 把用户选的设备套回去 —— 不做这一步,每次进房都会悄悄回到默认设备。
        // (顺序有保证:引擎初始化走的是 worker 线程上的 BlockingCall,
        //  在 CreatePeerConnection 返回前就已经做完了。)
        LOG_INFO("WebRTC") << "引擎初始化,使用用户选择的音频设备";
        applyDevice(true, m_desiredRecordingName, /*force=*/true);
        applyDevice(false, m_desiredPlayoutName, /*force=*/true);
    }

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
// 音频设备(麦克风 / 听筒)
// ============================
//
// 这一节直接和 WebRTC 的 ADM(AudioDeviceModule)打交道,有几处反直觉的地方,
// 全都能在 libwebrtc 源码里对上(路径以 /home/donggu/webrtc-checkout/src 为根)。
// 动这里之前先看一遍:
//
// 1) 声卡是**惰性初始化**的。
//    建第一条 PeerConnection 时 ConnectionContext::AddRefMediaEngine()
//    (pc/connection_context.cc:233)才 media_engine_->Init() → adm_helpers::Init()
//    → adm->Init()。在那之前枚举设备一律返回 -1(audio_device_impl.cc:48 的
//    CHECKinitialized_ 直接 return -1)。而设置页要能在**没进房间**时看到设备
//    列表,所以 ensureVoiceAdmOnWorker() 自己补一次 Init() —— 它是幂等的,
//    已初始化时直接返回 0(audio_device_impl.cc:287)。
//
// 2) 引擎每次(重新)初始化都会**把设备复位成 0**。
//    adm_helpers::Init() 里写死了 SetPlayoutDevice(0)/SetRecordingDevice(0)
//    (media/engine/adm_helpers.cc:43、63)。所以用户选的设备必须在建起第一条
//    连接之后重新套用一次,否则每次进房都悄悄回到系统默认 ——
//    见 createPeerConnection() 里那次 applyDevice(force=true)。
//
// 3) 播放/录音已经开始时改设备会**直接失败**。
//    PulseAudio 的 ADM 里,SetPlayoutDevice() 在 _playIsInitialized 时 return -1、
//    SetRecordingDevice() 在 _recIsInitialized 时 return -1
//    (modules/audio_device/linux/audio_device_pulse_linux.cc:677、798)。
//    所以运行中切换必须走 Stop → Set → Init → Start。真正打开设备是在
//    StartXxx()(连 pa stream 发生在音频线程,同文件 2013 起),
//    SetXxxDevice 只是把索引记下。
//
// 4) 所有 ADM 调用都压到 voice 的 worker 线程上,理由见 runOnVoiceWorker。
//
// 5) Linux 上**没有 GUID 可用**:PlayoutDeviceName/RecordingDeviceName 只填
//    name,guid 只被 memset 清零(同文件 716、754)。设备只能按名字认。

void WebrtcManager::runOnVoiceWorker(const std::function<void()> &fn) {
    if (m_voiceWorkerThread == nullptr) {
        LOG_WARN("WebRTC") << "voice worker 线程不存在,音频设备操作已跳过";
        return;
    }

    // 已经在这条线程上就直接跑 —— 投递给自己会死锁。
    if (m_voiceWorkerThread->IsCurrent()) {
        fn();
        return;
    }

    // 同步投递,等它跑完再返回。
    //
    // 为什么值得阻塞:这些操作本来就是阻塞的(Start/StopPlayout 内部最多等 10 秒),
    // 换设备又是用户偶尔点一下的动作 —— 同步拿结果,比异步回调回来再对状态少一堆
    // 竞态。更重要的是它把调用和 AudioState 排进**同一条队列**:AudioState 也在
    // worker 线程上,会根据有没有收发流自己调 Start/StopPlayout,交错起来就会出现
    // "我们刚 Stop,它转头就 Start"。
    m_voiceWorkerThread->BlockingCall([&fn] { fn(); });
}

bool WebrtcManager::ensureVoiceAdmOnWorker() {
    if (m_voiceAdm == nullptr) {
        LOG_WARN("WebRTC") << "voice 声卡不存在,无法操作音频设备";
        return false;
    }

    if (m_voiceAdm->Initialized())
        return true;

    const int32_t result = m_voiceAdm->Init();
    if (result != 0) {
        LOG_ERROR("WebRTC") << "声卡初始化失败(返回" << result << "),设备列表会是空的";
        return false;
    }

    LOG_INFO("WebRTC") << "声卡已初始化";
    return true;
}

std::vector<WebrtcManager::DeviceInfo> WebrtcManager::enumerateDevicesOnWorker(bool recording) {
    std::vector<DeviceInfo> devices;

    if (!ensureVoiceAdmOnWorker())
        return devices;

    const char *what = recording ? "录音" : "播放";

    const int16_t count = recording ? m_voiceAdm->RecordingDevices()
                                    : m_voiceAdm->PlayoutDevices();
    if (count <= 0) {
        LOG_WARN("WebRTC") << "枚举" << what << "设备失败(返回" << count << ")";
        return devices;
    }

    devices.reserve(static_cast<size_t>(count));
    for (uint16_t i = 0; i < static_cast<uint16_t>(count); ++i) {
        char name[128] = {0};   // kAdmMaxDeviceNameSize
        char guid[128] = {0};   // Linux 上恒为空,传进去只为凑接口
        const int32_t result = recording
                                   ? m_voiceAdm->RecordingDeviceName(i, name, guid)
                                   : m_voiceAdm->PlayoutDeviceName(i, name, guid);
        if (result != 0) {
            LOG_WARN("WebRTC") << "取" << what << "设备[" << i << "]的名字失败(返回" << result << ")";
            continue;
        }
        devices.push_back({static_cast<int>(i), QString::fromUtf8(name)});
    }

    QStringList names;
    names.reserve(static_cast<int>(devices.size()));
    for (const DeviceInfo &device : devices)
        names << device.name;
    LOG_INFO("WebRTC") << what << "设备" << devices.size() << "个:"
                       << (names.isEmpty() ? QStringLiteral("(无)")
                                           : names.join(QStringLiteral(" / ")));

    return devices;
}

bool WebrtcManager::applyRecordingDeviceOnWorker(uint16_t index) {
    if (!ensureVoiceAdmOnWorker())
        return false;

    // 录音已经初始化时不能直接改 —— PulseAudio 的 ADM 会返回 -1。
    // 先停掉,让它内部的状态落回"未初始化",Set 才吃得下。
    const bool wasInitialized = m_voiceAdm->RecordingIsInitialized();
    const bool wasRecording = m_voiceAdm->Recording();

    if (wasInitialized)
        m_voiceAdm->StopRecording();

    const int32_t setResult = m_voiceAdm->SetRecordingDevice(index);
    if (setResult != 0) {
        LOG_ERROR("WebRTC") << "选择麦克风设备索引" << index << "失败(返回" << setResult << ")";
        // 恢复录音
        if (wasInitialized && m_voiceAdm->InitRecording() == 0 && wasRecording)
            m_voiceAdm->StartRecording();
        return false;
    }

    if (wasInitialized) {
        if (m_voiceAdm->InitRecording() != 0)
            LOG_ERROR("WebRTC") << "切换后重新初始化录音失败,麦克风可能不出声";
        else if (wasRecording && m_voiceAdm->StartRecording() != 0)
            LOG_ERROR("WebRTC") << "切换后重新开始录音失败,麦克风可能不出声";
    }

    LOG_INFO("WebRTC") << "麦克风已切到设备索引" << index
                       << (wasRecording ? "(热切换)"
                                        : "");
    return true;
}

bool WebrtcManager::applyPlayoutDeviceOnWorker(uint16_t index) {
    if (!ensureVoiceAdmOnWorker())
        return false;

    const bool wasInitialized = m_voiceAdm->PlayoutIsInitialized();
    const bool wasPlaying = m_voiceAdm->Playing();

    if (wasInitialized)
        m_voiceAdm->StopPlayout();

    const int32_t setResult = m_voiceAdm->SetPlayoutDevice(index);
    if (setResult != 0) {
        LOG_ERROR("WebRTC") << "选择听筒设备索引" << index << "失败(返回" << setResult << ")";
        if (wasInitialized && m_voiceAdm->InitPlayout() == 0 && wasPlaying)
            m_voiceAdm->StartPlayout();
        return false;
    }

    if (wasInitialized) {
        if (m_voiceAdm->InitPlayout() != 0)
            LOG_ERROR("WebRTC") << "切换后重新初始化播放失败,可能听不到对方语音";
        else if (wasPlaying && m_voiceAdm->StartPlayout() != 0)
            LOG_ERROR("WebRTC") << "切换后重新开始播放失败,可能听不到对方语音";
    }

    LOG_INFO("WebRTC") << "听筒已切到设备索引" << index
                       << (wasPlaying ? "(热切换)"
                                      : "");
    return true;
}

void WebrtcManager::reloadDevices() {
    std::vector<DeviceInfo> recording;
    std::vector<DeviceInfo> playout;

    // 枚举要在 worker 线程上做(ADM 的线程约定),结果拷回来在主线程存下来 ——
    // 这样 m_recordingDevices / m_playoutDevices 只有主线程碰,不用加锁。
    runOnVoiceWorker([&] {
        recording = enumerateDevicesOnWorker(true);
        playout = enumerateDevicesOnWorker(false);
    });

    if (recording == m_recordingDevices && playout == m_playoutDevices)
        return;

    m_recordingDevices = std::move(recording);
    m_playoutDevices = std::move(playout);
    emit audioDevicesChanged();
}

int WebrtcManager::currentDeviceIndex(bool recording) const {
    return recording ? m_recordingDeviceIndex : m_playoutDeviceIndex;
}

const std::vector<WebrtcManager::DeviceInfo> &WebrtcManager::deviceList(bool recording) const {
    return recording ? m_recordingDevices : m_playoutDevices;
}

bool WebrtcManager::applyDevice(bool recording, const QString &name, bool force) {
    if (m_voiceAdm == nullptr || !m_initialized) {
        LOG_INFO("WebRTC") << (recording ? "麦克风" : "听筒")
                           << "设备选择已记下,进房间后再套用";
        return false;
    }

    // 还没枚举过就先补一次 —— 把名字解析成索引得有设备表
    if (m_recordingDevices.empty() || m_playoutDevices.empty())
        reloadDevices();

    const int index = indexOfDeviceName(name, deviceList(recording));
    if (!name.isEmpty() && index == 0) {
        LOG_WARN("WebRTC") << (recording ? "麦克风" : "听筒") << "设备\"" << name
                           << "\"不在设备表里,退回系统默认";
    }

    // 目标就是现状,不用动 —— 免得白停一次播放/录音
    if (!force && index == currentDeviceIndex(recording))
        return true;

    const uint16_t target = static_cast<uint16_t>(index);
    bool ok = false;
    runOnVoiceWorker([&] {
        ok = recording ? applyRecordingDeviceOnWorker(target)
                       : applyPlayoutDeviceOnWorker(target);
    });
    if (!ok)
        return false;

    if (recording)
        m_recordingDeviceIndex = index;
    else
        m_playoutDeviceIndex = index;
    return true;
}

bool WebrtcManager::setDesiredDevice(bool recording, const QString &name) {
    QString &desired = recording ? m_desiredRecordingName : m_desiredPlayoutName;

    const bool changed = (desired != name);
    desired = name;

    // 语音那一路:落到 ADM 上
    const bool applied = applyDevice(recording, name, /*force=*/false);

    if (changed) {
        // 听筒还有第二处出声的地方 —— 电影声(MovieAudioPlayer 的 QAudioSink)。
        // 它不受 WebRTC 引擎初始化影响,所以和 ADM 那边分开处理;
        // 只在真的变了的时候动它,否则每次启动都会白重启一次音频输出
        if (!recording && m_movieAudioPlayer != nullptr)
            m_movieAudioPlayer->setOutputDevice(findAudioOutputByName(name));

        if (recording)
            emit recordingDeviceChanged(name);
        else
            emit playoutDeviceChanged(name);
    }

    return applied;
}

QVariantList WebrtcManager::recordingDevices() const {
    return devicesToVariant(m_recordingDevices);
}

QVariantList WebrtcManager::playoutDevices() const {
    return devicesToVariant(m_playoutDevices);
}

QString WebrtcManager::recordingDeviceName() const {
    return m_desiredRecordingName;
}

QString WebrtcManager::playoutDeviceName() const {
    return m_desiredPlayoutName;
}

void WebrtcManager::refreshAudioDevices() {
    reloadDevices();

    // 上次选的设备要是不在了(拔了),明确回到"跟随系统默认" ——
    // 否则界面上一个都不高亮,看起来像坏了。顺带把还在用它的 ADM 一起退回去。
    for (const bool recording : {true, false}) {
        const QString desired = recording ? m_desiredRecordingName : m_desiredPlayoutName;
        if (desired.isEmpty())
            continue;
        if (indexOfDeviceName(desired, deviceList(recording)) != 0)
            continue;

        LOG_WARN("WebRTC") << (recording ? "麦克风" : "听筒") << "设备\"" << desired
                           << "\"已经不在了,回到系统默认";
        setDesiredDevice(recording, QString());
    }
}

bool WebrtcManager::selectRecordingDevice(const QString &name) {
    return setDesiredDevice(true, name);
}

bool WebrtcManager::selectPlayoutDevice(const QString &name) {
    return setDesiredDevice(false, name);
}

// ============================
// 电影音频
// ============================

bool WebrtcManager::movieAudioEnabled() const {
    return m_localMovieAudioTrack != nullptr && m_localMovieAudioTrack->enabled();
}

void WebrtcManager::setLocalVideoEnabled(bool enabled) {
    if (!getOrCreateVideoTrack()) {
        // 没有视频轨(初始化失败)时不用报错:调用方是"按模式门控"的逻辑,
        // 拿不到轨就是没得发,静默即可
        return;
    }

    if (m_localVideoTrack->enabled() == enabled)
        return;

    m_localVideoTrack->set_enabled(enabled);

    // 切模式时才会走这里,频率很低,放 Info
    LOG_INFO("WebRTC") << "推送视频已" << (enabled ? "开启" : "关闭") << "(当前"
                       << m_peers.size() << "个对端)";
}

void WebrtcManager::setVideoQuality(VideoQuality quality) {
    if (m_videoQuality == quality && !m_peers.isEmpty())
        return;

    m_videoQuality = quality;

    const VideoQualityPreset preset = presetForQuality(quality);
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        if (it.value().media != nullptr)
            it.value().media->applyVideoQuality(preset.maxBitrateBps,
                                                preset.maxFramerate);
    }

    LOG_INFO("WebRTC") << "推送画质设为" << qualityName(quality)
                       << "(码率上限" << (preset.maxBitrateBps / 1000) << "kbps)";
    emit videoQualityChanged(quality);
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
        m_voiceAdm,
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

    // ---- 建那条"保活连接"(声卡不被关掉的唯一理由)----
    //
    // 背景:WebRTC 的 media engine(持有我们这块真声卡)按引用计数关闭,
    // 引用它的只有 PeerConnection(pc/connection_context.cc 的
    // MediaEngineReference)。离开房间时所有对端都被销毁 → 引用归零 →
    // WebRtcVoiceEngine::Terminate() → adm()->Terminate()。
    //
    // 正常情况下这没什么 —— 下次进房引擎会重新 Init 声卡。但 PulseAudio
    // 那一支上这一刀是**不可逆的**:
    //   modules/audio_device/linux/audio_device_pulse_linux.cc
    //   · Terminate() 把 quit_ 置为 true;
    //   · 只有**构造函数**会把它复位成 false;
    //   · Init() 重新拉起的播放/录音线程,一睁眼就是
    //     "if (quit_) return false;"(同文件 PlayThreadProcess/RecThreadProcess),
    //     于是线程立刻退出。
    // 结果:StartPlayout()/StartRecording() 永远等不到启动事件,10 秒后
    // 返回 -1 —— 第二次进房之后麦克风和听筒**全都哑掉**。
    //
    // 实测(一次性自检程序,同一进程内):建对端 → 声卡初始化 true;
    // 销毁全部对端 → 声卡初始化 false;再建对端 → 初始化又 true,
    // 但 StartPlayout 与 StartRecording 双双返回 -1。
    //
    // 所以这里留一条什么都不做的连接活着,引用计数就永远 ≥ 1,引擎不关声卡,
    // 这个坑便绕过去了。它是给上游 bug 打的补丁,不该被"清理掉"。
    // (media 工厂不需要:那边是假声卡,根本没有线程。)
    webrtc::PeerConnectionInterface::RTCConfiguration keepAliveConfig;
    webrtc::PeerConnectionDependencies keepAliveDeps(&m_voiceKeepAliveObserver);
    auto keepAliveResult =
        m_voiceFactory->CreatePeerConnectionOrError(keepAliveConfig, std::move(keepAliveDeps));
    if (keepAliveResult.ok()) {
        m_voiceKeepAlive = keepAliveResult.value();
        LOG_INFO("WebRTC") << "已建立常驻保活连接(防止退房时声卡被永久关掉)";
    } else {
        // 没建成也不算致命:只是"离开房间后再进房会没声音"那个老问题会回来
        LOG_WARN("WebRTC") << "保活连接创建失败:"
                           << QString::fromStdString(keepAliveResult.error().message())
                           << "(退房后再进房可能没声音)";
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

    // 保活连接也是连接,和解房间里的对端一样要放在工厂之前释放。
    // 走到这一步说明整个 WebRTC 世界都在拆,不需要它再撑着声卡了。
    m_voiceKeepAlive = nullptr;

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

    // 4. 两块声卡和环境放最后:工厂已经不再持有它们了。
    //    **ADM 比 Environment 先放** —— 它们内部持有对环境的引用
    //    (Environment 是值类型、各自存了一份,但顺序颠倒仍然容易踩坑)。
    m_dummyAdm = nullptr;
    m_voiceAdm = nullptr;
    m_mediaEnvironment.reset();

    m_initialized = false;
}
