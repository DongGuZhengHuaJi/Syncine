//
// Created by donggu on 2026/9/10.
//

#include "WebrtcManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
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
#include "rtc_base/network.h"

// ----------------------------------------------------------------------
// ABI 护栏：客户端看到的类布局必须与 libwebrtc.a 编译时一致。
//
// WebRTC 头文件中存在由 `#if RTC_DCHECK_IS_ON` 包裹的条件成员
// （见 rtc_base/thread.h 中 webrtc::Thread 的字段声明）。该宏在两侧不一致时，
// webrtc::Thread 等类在客户端与库中的 sizeof 不同、成员偏移整体错位，
// 症状为运行时随机段错误，极难定位。
//
// libwebrtc.a 为 Release 构建（is_debug = false），RTC_DCHECK_IS_ON 必须为 0，
// 由 Client/CMakeLists.txt 第 7 节的 NDEBUG 保证。此处编译失败时：
//   - 若确实将 WebRTC 重编为带 DCHECK 的版本，请同步移除该处的 NDEBUG
//   - 否则即为 NDEBUG 被误删，需加回
// ----------------------------------------------------------------------
#if RTC_DCHECK_IS_ON
#error "RTC_DCHECK_IS_ON 应为 0：libwebrtc.a 为 Release（is_debug = false）构建，详见 Client/CMakeLists.txt 第 7 节"
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
        LOG_ERROR("WebRTC") << which << "线程启动失败，正在停止已启动的线程";
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

// 服务端未提供任何配置时的兜底（旧版服务端，或其字段未解析出来）。
// 使用一个公共 STUN：无法得知自身公网映射地址时，STUN 至少能探测出来，
// 局域网与普通 NAT 仍可直连，缺少的只是对称 NAT 下的中继。
//
// STUN 的 URL 不能带 query。libwebrtc 的 ICE URL 解析明确拒绝
// "stun:host:port?transport=tcp" 这类写法，且属硬错误：任一 URL 不合法即导致
// 整条 PeerConnection 无法建立（不会跳过该项），症状仅为 PeerLink 中的一句
// 「连接创建失败」，难以联想到此处。
const QList<IceServerConfig> &defaultIceServers() {
    static const QList<IceServerConfig> servers = [] {
        IceServerConfig stun;
        stun.urls = {QStringLiteral("stun:stun.cloudflare.com:3478")};
        return QList<IceServerConfig>{stun};
    }();
    return servers;
}

// ----------------------------------------------------------------------
// 立体声 Opus 编解码器工厂包装——media 工厂专用
//
// SDP 中 Opus 的「2 声道」与「立体声」是两个概念：rtpmap 的 opus/48000/2 仅表示
// 「最多 2 声道」，决定编码器声道数的是 fmtp 中的 stereo=1（audio_encoder_opus.cc
// 的 GetChannelCount）。内置工厂不带该参数，故协商结果恒为单声道，推入立体声 PCM
// 亦被 ACM 下混（ReMixFrame）。解法：补上 stereo=1 使其进入 SDP fmtp
// （media/base/codec.cc 的 Codec(SdpAudioFormat) 会拷贝全部参数）。
//
// 编解码器两个工厂都必须包装：电影音轨的 m-line 为 sendrecv，codec 列表由收发两个
// 列表合并，参数取自 recv 列表那份（pc/codec_vendor.cc），仅包装编码器工厂时
// stereo=1 位于 send 列表、合并时被丢弃（实测）。
// 仅对 media 工厂包装：麦克风为单声道，voice 工厂协商立体声只会无谓上混。
//
// maxaveragebitrate：Opus 默认码率（约 32kbps）对音乐偏低，提高到 64kbps；
// 可调范围为 6~510kbps（见同文件 CalculateBitrate 钳制），同样需两处都写。
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
        // 内置工厂本已识别 stereo/maxaveragebitrate 参数，直接转发
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

// 解码器侧的同类包装，原因见上方说明
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
// 码率/帧率上限 + 显式目标分辨率。低档位不再让编码器自己猜降到多少。
// 关于「原画」：WebRTC 中不设上限并非无限，未设置时会采用按分辨率计算的默认上限
// （encoder_stream_factory.cc 的 GetMaxDefaultVideoBitrateKbps：540p 以上一律
// 2500kbps），比「高清」还低；故「原画」须显式给出较高上限，实际上限交由带宽估计。
struct VideoQualityPreset {
    int maxBitrateBps = 0;   // 0 = 用 WebRTC 默认上限
    double maxFramerate = 0; // 0 = 不限制
    int targetHeight = 0;    // 0 = 不限制分辨率（按源尺寸）
};

VideoQualityPreset presetForQuality(WebrtcManager::VideoQuality quality) {
    switch (quality) {
    case WebrtcManager::VideoQuality::Smooth:
        // 流畅：480p，帧率留 24(电影本身也就 24)
        return {800'000, 24.0, 480};
    case WebrtcManager::VideoQuality::Standard:
        // 标准：720p
        return {2'000'000, 30.0, 720};
    case WebrtcManager::VideoQuality::Hd:
        // 高清：1080p。已高于 WebRTC 的默认上限（2.5Mbps）
        return {4'000'000, 30.0, 1080};
    case WebrtcManager::VideoQuality::Original:
        // 原画：上限设得很高，实际速率由网络决定；不限制帧率/分辨率
        return {10'000'000, 0.0, 0};
    }
    return {10'000'000, 0.0, 0};
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

// 诊断开关：SYNCINE_ICE_IGNORE_VPN=1 时不收集 VPN/虚拟网卡候选。
// Radmin/ZeroTier/Tailscale 这类网卡常被 ICE 选中但实际不通，表现为
// “换热点/校园网才正常”。默认不开启：有人确实只能走 VPN。
void applyFactoryNetworkOptions(
    const webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> &factory)
{
    if (factory == nullptr || !qEnvironmentVariableIsSet("SYNCINE_ICE_IGNORE_VPN"))
        return;

    webrtc::PeerConnectionFactoryInterface::Options options;
    options.network_ignore_mask =
        webrtc::kDefaultNetworkIgnoreMask | webrtc::ADAPTER_TYPE_VPN;
    factory->SetOptions(options);
    LOG_INFO("WebRTC") << "已忽略 VPN/虚拟网卡候选(SYNCINE_ICE_IGNORE_VPN)";
}

// ---- 音频设备的两个小工具 ----

// 按名字在设备表中查找索引。名字为空（= 跟随系统默认）或未找到时均返回 kDefaultDevice。
//
// 未找到时退回默认而非报错：设备被拔出是常态（上次使用的 USB 耳机本次未插入），
// 退回「跟随系统默认」优于完全不出声。
int indexOfDeviceName(const QString &name,
                      const std::vector<WebrtcManager::DeviceInfo> &devices) {
    if (name.isEmpty())
        return WebrtcManager::kDefaultDevice;

    for (const WebrtcManager::DeviceInfo &device : devices) {
        if (device.name == name)
            return device.index;
    }
    return WebrtcManager::kDefaultDevice;
}

// 设备表 → OptionList 所需的 [{label, value}]。
//
// 首项固定是「跟随系统默认」（value 为空串），而不是把 ADM 的 0 号当作默认：
// 0 号在 PulseAudio 上是 ADM 自己插的 "default: xxx" 伪设备（名字随系统默认变化，
// 存下来会失效），在 WASAPI 上却是一个真实端点（枚举出来的第一个，与系统默认
// 毫无约定关系）。故 Linux 上跳过 0 号这一项（语义与首项重复），Windows 上保留。
QVariantList devicesToVariant(const std::vector<WebrtcManager::DeviceInfo> &devices) {
    QVariantList list;

    QVariantMap defaultItem;
    defaultItem[QStringLiteral("label")] = QStringLiteral("跟随系统默认");
    defaultItem[QStringLiteral("value")] = QString();
    list.append(defaultItem);

    for (const WebrtcManager::DeviceInfo &device : devices) {
#if !defined(Q_OS_WIN)
        if (device.index == 0)
            continue;
#endif
        QVariantMap item;
        item[QStringLiteral("label")] = device.name;
        item[QStringLiteral("value")] = device.name;
        list.append(item);
    }
    return list;
}

// 「跟随系统默认」落到 ADM 上。这是全项目唯一按平台分叉的地方，两条路完全不同：
//
//   Linux/PulseAudio：设备表 0 号就是 ADM 插进来的 "default: xxx" 伪设备，开流时
//     重新解析当前默认 sink（audio_device_pulse_linux.cc:255、:703），故传索引 0
//     即「跟着系统默认走」。注意不能改用下面那个 WindowsDeviceType 重载——Pulse
//     与 ALSA 都直接返回 -1（"WindowsDeviceType not supported"，同文件 :697、:819）。
//
//   Windows/WASAPI：设备表就是 EnumAudioEndpoints 的原样集合，0 号是「枚举出来的
//     第一个端点」，与系统默认没有任何约定关系（ADM 自己想知道默认排第几，也要
//     遍历集合比对设备 ID，见 _GetDefaultDeviceIndex）。默认设备只能用另一个重载
//     按角色查 GetDefaultAudioEndpoint。传索引 0 会静默播到一个用户听不见的设备上：
//     SetXxxDevice(0) 完全合法、不报错，声音只是进了别处（实测：Steam 虚拟声卡）。
//
// 角色取 kDefaultCommunicationDevice（「默认通信设备」）而非 kDefaultDevice
// （「系统默认设备」）：与 WebRTC 引擎自己初始化时的选择保持一致（adm_helpers.cc:30），
// 于是「跟随系统默认」这一步在建起第一条连接后重套时是个语义空操作，不会把引擎
// 刚设好的设备改到别处。两者在没单独设过通信设备的机器上是同一个设备。
int32_t setDefaultDevice(webrtc::AudioDeviceModule *adm, bool recording) {
#if defined(Q_OS_WIN)
    const auto role = webrtc::AudioDeviceModule::kDefaultCommunicationDevice;
    return recording ? adm->SetRecordingDevice(role) : adm->SetPlayoutDevice(role);
#else
    return recording ? adm->SetRecordingDevice(0) : adm->SetPlayoutDevice(0);
#endif
}

// 「默认设备」在设备名查询接口里的索引，与上面的 setDefaultDevice 配套：
//   Windows：ADM 把索引 (uint16_t)-1 特判成默认通信设备（audio_device_core_win.cc:1490），
//            与 kDefaultCommunicationDevice 取到的正是同一个设备；
//   Linux  ：0 号伪设备的名字即 "default: <当前默认 sink>"。
uint16_t defaultDeviceQueryIndex() {
#if defined(Q_OS_WIN)
    return static_cast<uint16_t>(-1);
#else
    return 0;
#endif
}

// 取设备名，仅用于日志。取不到时返回空串。
//
// 为什么值得专门记一行：设备选错是**静默**故障——SetXxxDevice 一律返回 0，
// 不比对一下名字，事后无从判断声音到底进了哪个设备。
QString deviceNameAt(webrtc::AudioDeviceModule *adm, bool recording, uint16_t index) {
    if (adm == nullptr)
        return {};

    char name[128] = {0};   // kAdmMaxDeviceNameSize
    char guid[128] = {0};   // Linux 上恒为空,传进去只为凑接口
    const int32_t result = recording
                               ? adm->RecordingDeviceName(index, name, guid)
                               : adm->PlayoutDeviceName(index, name, guid);
    if (result != 0)
        return {};
    return QString::fromUtf8(name);
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

    // 在 voice 与 media 工厂初始化之前创建环境与 ADM
    m_mediaEnvironment.emplace(webrtc::CreateEnvironment());

    m_voiceAdm = webrtc::CreateAudioDeviceModule(*m_mediaEnvironment, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
    if (m_voiceAdm == nullptr) {
        LOG_ERROR("WebRTC") << "创建 voice 声卡失败，放弃初始化";
        m_mediaEnvironment.reset();
        return false;
    }
    m_dummyAdm = webrtc::CreateAudioDeviceModule(*m_mediaEnvironment, webrtc::AudioDeviceModule::kDummyAudio);
    if (m_dummyAdm == nullptr) {
        LOG_ERROR("WebRTC") << "创建 media 声卡失败，放弃初始化";
        m_mediaEnvironment.reset();
        return false;
    }

    if (!initializeVoiceFactory()) {
        LOG_ERROR("WebRTC") << "初始化 voice 工厂失败，放弃初始化";
        // 两个 ADM 都引用了 Environment，必须早于它释放
        m_voiceAdm = nullptr;
        m_dummyAdm = nullptr;
        m_mediaEnvironment.reset();
        return false;
    }

    if (!initializeMediaFactory()) {
        LOG_ERROR("WebRTC") << "初始化 media 工厂失败，放弃初始化";
        m_dummyAdm = nullptr;
        m_mediaEnvironment.reset();
        destroy();
        return false;
    }

    m_initialized = true;
    LOG_INFO("WebRTC") << "WebRTC 初始化成功";

    // 预创建各条轨道
    if (getOrCreateAudioTrack() == nullptr) {
        LOG_WARN("WebRTC") << "推流语音轨预创建失败，本次运行将无法共享麦克风声音";
    }
    if (getOrCreateVideoTrack() == nullptr) {
        LOG_WARN("WebRTC") << "推流视频轨预创建失败，本次运行将无法共享视频画面";
    }
    if (getOrCreateMovieAudioTrack() == nullptr) {
        LOG_WARN("WebRTC") << "推流电影音轨预创建失败，本次运行将没有共享视频声音";
    }

    // 预创建共享模式下接收侧的画面渲染器与声音播放器。
    // 渲染器按“当前音频还要多久响出”给画面排期：回调里只读播放器的估计值，
    // 不持有它的裸指针；destroy() 时会先清回调再销毁两者。
    m_remoteRenderer = std::make_unique<RemoteVideoRenderer>();
    m_movieAudioPlayer = std::make_unique<MovieAudioPlayer>();
    m_movieAudioPlayer->setVolume(m_remoteMovieVolume);
    m_remoteRenderer->setPlayoutDelayProvider([this]() {
        return m_movieAudioPlayer != nullptr ? m_movieAudioPlayer->playoutDelayMs() : 0;
    });

    return true;
}

// ============================
// ICE 配置
// ============================

void WebrtcManager::setIceServers(const QList<IceServerConfig> &servers) {
    // 成员每次变化都会走一遍「建连前刷新配置」，内容未变时不重复记录日志
    if (servers == m_iceServers)
        return;
    m_iceServers = servers;

    if (servers.isEmpty()) {
        LOG_WARN("WebRTC") << "服务端未下发 ICE 配置，退回兜底 STUN；"
                              "对称 NAT 后的对端可能无法连接";
        return;
    }

    int urlCount = 0;
    bool hasTurn = false;
    for (const IceServerConfig &server : servers) {
        urlCount += server.urls.size();
        for (const QString &url : server.urls)
            hasTurn = hasTurn || url.startsWith(QLatin1String("turn"));
    }

    // 只记录条数：username/credential 是临时密钥，任何级别都不写入日志
    LOG_INFO("WebRTC") << "ICE 配置已更新:" << servers.size() << "组服务器 /"
                       << urlCount << "个 URL" << (hasTurn ? "（含 TURN）" : "（仅 STUN）");
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
        // 协议中名为 credential（Cloudflare 的字段名），libwebrtc 的 IceServer
        // 中名为 password，实为同一项，全项目只在此处转换一次。
        server.password = entry.credential.toStdString();

        servers.push_back(std::move(server));
    }

    // 额外补一个双栈 STUN：服务端自建 coturn 的 STUN 往往只有 IPv4，
    // libwebrtc 对 IPv6 host 候选会报 "STUN server address is incompatible"，
    // 拿不到 IPv6 srflx。Cloudflare STUN 有 AAAA，能帮助两端走 IPv6 直连
    // （尤其是手机热点这种 IPv4 在 CGNAT 后的网络）。不可达时只是多一条
    // 候选错误，已在 WebRTC 日志里降噪。
    bool hasDualStackStun = false;
    for (const auto &server : servers) {
        for (const std::string &url : server.urls) {
            if (url.find("stun.cloudflare.com") != std::string::npos) {
                hasDualStackStun = true;
                break;
            }
        }
        if (hasDualStackStun)
            break;
    }
    if (!hasDualStackStun) {
        webrtc::PeerConnectionInterface::IceServer extra;
        extra.urls.push_back("stun:stun.cloudflare.com:3478");
        servers.push_back(std::move(extra));
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
        LOG_ERROR("WebRTC") << (isVoice ? "voice" : "media") << " 工厂为空，无法建立连接";
        return nullptr;
    }

    // ICE 配置（STUN + 可能带临时凭据的 TURN）在进房时由 SignalingChannel 传入，
    // 见 setIceServers。配置在下方 CreatePeerConnection 时生效——
    // 此后修改 m_iceServers 不会影响这条连接。
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.servers = buildIceServers();

    // 诊断开关：SYNCINE_ICE_RELAY_ONLY=1 时只收集 relay 候选。
    // 用于确认是否真正走 TURN——此时只有 TURN 可用才能连上，
    // 能直连说明不了问题（局域网内本就能连通）。默认关闭，不要长期开启。
    if (qEnvironmentVariableIsSet("SYNCINE_ICE_RELAY_ONLY"))
        config.type = webrtc::PeerConnectionInterface::kRelay;

    auto *link = new PeerLink(peerId, kind, factory, config, createDataChannel, this);
    if (!link->isValid()) {
        LOG_ERROR("WebRTC") << (isVoice ? "voice" : "media") << " 连接创建失败，已放弃";
        delete link;
        return nullptr;
    }

    // 在 createOffer 之前挂上本地轨，SDP 中才会包含相应的 m=audio/m=video 段
    // TODO: 完善重新协商逻辑
    if (isVoice) {
        if (m_localAudioTrack != nullptr)
            link->addLocalAudioTrack(m_localAudioTrack);
        link->setRemoteAudioVolume(m_remoteChatVolume);

        // 数据通道只挂在 voice 连接上，原样转发给上层
        connect(link, &PeerLink::dataChannelOpened,
                this, &WebrtcManager::dataChannelOpened);
        connect(link, &PeerLink::dataChannelClosed,
                this, &WebrtcManager::dataChannelClosed);
        connect(link, &PeerLink::dataChannelMessageReceived,
                this, &WebrtcManager::dataChannelMessageReceived);
    } else {
        if (m_localVideoTrack != nullptr) {
            link->addLocalVideoTrack(m_localVideoTrack, m_videoSending);
            // 新连接套用当前画质档位（观众可能在房主修改档位之后才进入）
            applyQualityToLink(link);
        }
        if (m_localMovieAudioTrack != nullptr) {
            // 初始 active 直接写进 init_send_encodings，协商时发送流
            // 从一开始就是停的（非共享模式）
            link->addLocalMovieAudioTrack(m_localMovieAudioTrack,
                                          m_movieAudioSending);
        }

        // 接收侧：远端画面送入共享渲染器
        if (m_remoteRenderer != nullptr)
            link->setRemoteVideoSink(m_remoteRenderer.get());

        // 接收侧：远端电影声送入自行实现的播放器
        if (m_movieAudioPlayer != nullptr)
            link->setRemoteAudioSink(m_movieAudioPlayer.get());

        // 关闭 media 连接的播放，交由 NullAudioPoller 接管，
        // 每 10ms 自动拉取音频数据送入 sink
        link->setPlayoutEnabled(false);
    }

    return link;
}

bool WebrtcManager::createPeerConnection(const QString &peerId, bool createDataChannel) {
    if (!m_initialized) {
        LOG_ERROR("WebRTC") << "尚未初始化，无法建立对端连接";
        return false;
    }

    if (peerId.isEmpty()) {
        LOG_WARN("WebRTC") << "对端 ID 为空，无法建立连接";
        return false;
    }

    // 幂等：已存在则直接返回 true，不重复创建两条连接
    if (m_peers.contains(peerId))
        return true;

    PeerLinks links;
    links.voice = createLink(peerId, LinkKind::Voice, createDataChannel);
    links.media = createLink(peerId, LinkKind::Media, /*createDataChannel=*/false);

    if (links.voice == nullptr || links.media == nullptr) {
        LOG_ERROR("WebRTC") << "对端 " << peerId << " 的连接建立失败（voice:"
                            << (links.voice ? "ok" : "fail")
                            << " media:" << (links.media ? "ok" : "fail") << "）";
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

    // 插入前先判断：是否为本次进房的第一条连接（引擎即在此刻初始化）
    const bool firstPeer = m_peers.isEmpty();
    m_peers.insert(peerId, links);
    LOG_INFO("WebRTC") << "已建立对端连接:" << peerId << "（两条：voice + media，当前共"
                       << m_peers.size() << "个对端）";

    if (firstPeer) {
        // 上一步 CreatePeerConnection 内部会先启动引擎，而
        // adm_helpers::Init() 已按平台自行选过一次设备（见本节第 2 条）。
        // 需把用户选择的设备套回——缺少此步骤，每次进房都会静默回到默认设备。
        // （顺序有保证：引擎初始化走 worker 线程上的 BlockingCall，
        //  在 CreatePeerConnection 返回前即已完成。）
        LOG_DEBUG("WebRTC") << "引擎初始化，重新套用用户选择的音频设备";
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
        // 数据通道随之消失。通知上层结束进行中的文件传输
        emit peerRemoved(peerId);
    }
}

bool WebrtcManager::isDataChannelOpen(const QString &peerId,
                                      const QString &channel) const {
    PeerLink *link = peer(peerId, LinkKind::Voice);
    return link != nullptr && link->isDataChannelOpen(channel);
}

quint64 WebrtcManager::dataChannelBufferedAmount(const QString &peerId,
                                                 const QString &channel) const {
    PeerLink *link = peer(peerId, LinkKind::Voice);
    return link != nullptr ? link->dataChannelBufferedAmount(channel) : 0;
}

bool WebrtcManager::sendDataChannelMessage(const QString &peerId,
                                           const QString &channel,
                                           const QByteArray &data, bool binary) {
    PeerLink *link = peer(peerId, LinkKind::Voice);
    return link != nullptr && link->sendDataChannelMessage(channel, data, binary);
}

void WebrtcManager::requestDataChannelStats(const QString &peerId) {
    PeerLink *link = peer(peerId, LinkKind::Voice);
    if (link != nullptr)
        link->requestDataChannelStats();
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
        // 扬声器外放 + 麦克风同时工作的场景下，不消回声就是啸叫
        options.echo_cancellation = true;
        // 不同人说话的远近、音量差很大，自动增益能把它们拉平
        options.auto_gain_control = true;
        // 风扇、键盘、空调底噪
        options.noise_suppression = true;
        // 过滤低频轰鸣（桌面震动、空调）
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

    LOG_INFO("WebRTC") << (enabled ? "麦克风已开启" : "麦克风已关闭")
                       << "(当前" << m_peers.size() << "个对端)";
    emit audioEnabledChanged(enabled);
}

// ============================
// 音频设备（麦克风 / 听筒）
// ============================
//
// 本节直接与 WebRTC 的 ADM 交互，若干行为反直觉，均可在 libwebrtc 源码中核对
// （路径以 /home/donggu/webrtc-checkout/src 为根）。改动前需先阅读：
//
// 1) 声卡惰性初始化：建第一条 PeerConnection 时由 connection_context.cc:233 触发
//    media_engine_->Init() → adm->Init()；在此之前枚举设备一律返回 -1
//    （audio_device_impl.cc:48）。设置页需在未进房时看到设备列表，故
//    ensureVoiceAdmOnWorker() 自行补一次（该操作幂等，见同文件 :287）。
// 2) 引擎每次（重新）初始化都会自行选一次设备（adm_helpers.cc:43、63），把用户
//    所选的那个覆盖掉，故必须在建起第一条连接后重新套用，见 createPeerConnection()
//    中的 applyDevice(force=true)。它选的值按平台不同（同文件 :30 的宏）：
//    Windows 是 kDefaultCommunicationDevice（角色查询），其余平台才是索引 0。
//    这条差异正是「跟随系统默认」必须分平台实现的原因，见 setDefaultDevice()。
// 3) 播放/录音已启动时改设备会直接失败：PulseAudio 的 ADM 在 _playIsInitialized /
//    _recIsInitialized 时返回 -1（audio_device_pulse_linux.cc:677、798），运行中
//    切换必须走 Stop → Set → Init → Start；SetXxxDevice 仅记录索引，真正打开
//    设备发生在 StartXxx()。
// 4) 所有 ADM 调用压到 voice 的 worker 线程，理由见 runOnVoiceWorker。
// 5) Linux 上无 GUID 可用（guid 恒被清零，同文件 :716、754），设备只能按名字识别。

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

    // 同步投递，等它跑完再返回。
    //
    // 为什么值得阻塞：这些操作本来就是阻塞的（Start/StopPlayout 内部最多等 10 秒）,
    // 换设备又是用户偶尔点一下的动作 —— 同步拿结果，比异步回调回来再对状态少一堆
    // 竞态。更重要的是它把调用和 AudioState 排进**同一条队列**:AudioState 也在
    // worker 线程上，会根据有没有收发流自己调 Start/StopPlayout,交错起来就会出现
    // 「本端刚 Stop，它随即 Start」的竞态。
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
    LOG_DEBUG("WebRTC") << what << "设备" << devices.size() << "个:"
                       << (names.isEmpty() ? QStringLiteral("(无)")
                                           : names.join(QStringLiteral(" / ")));

    return devices;
}

bool WebrtcManager::applyRecordingDeviceOnWorker(int index) {
    if (!ensureVoiceAdmOnWorker())
        return false;

    // 录音已经初始化时不能直接改 —— PulseAudio 的 ADM 会返回 -1。
    // 先停掉，让它内部的状态落回"未初始化",Set 才吃得下。
    const bool wasInitialized = m_voiceAdm->RecordingIsInitialized();
    const bool wasRecording = m_voiceAdm->Recording();

    if (wasInitialized)
        m_voiceAdm->StopRecording();

    const bool useDefault = (index == kDefaultDevice);
    const int32_t setResult = useDefault
                                  ? setDefaultDevice(m_voiceAdm.get(), true)
                                  : m_voiceAdm->SetRecordingDevice(static_cast<uint16_t>(index));
    if (setResult != 0) {
        LOG_ERROR("WebRTC") << "选择麦克风设备"
                            << (useDefault ? QStringLiteral("「跟随系统默认」")
                                           : QString::number(index) + QStringLiteral(":"))
                            << "失败(返回" << setResult << ")";
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

    // 把解析结果一起记下：选错设备是静默故障，事后只能靠名字判断声音进了哪儿。
    const QString name = deviceNameAt(m_voiceAdm.get(), true,
                                      useDefault ? defaultDeviceQueryIndex()
                                                 : static_cast<uint16_t>(index));
    LOG_DEBUG("WebRTC") << "麦克风已切到"
                       << (useDefault ? QStringLiteral("跟随系统默认:")
                                      : QString::number(index) + QStringLiteral(":"))
                       << (name.isEmpty() ? QStringLiteral("(取不到设备名)") : name)
                       << (wasRecording ? "(热切换)" : "");
    return true;
}

bool WebrtcManager::applyPlayoutDeviceOnWorker(int index) {
    if (!ensureVoiceAdmOnWorker())
        return false;

    const bool wasInitialized = m_voiceAdm->PlayoutIsInitialized();
    const bool wasPlaying = m_voiceAdm->Playing();

    if (wasInitialized)
        m_voiceAdm->StopPlayout();

    const bool useDefault = (index == kDefaultDevice);
    const int32_t setResult = useDefault
                                  ? setDefaultDevice(m_voiceAdm.get(), false)
                                  : m_voiceAdm->SetPlayoutDevice(static_cast<uint16_t>(index));
    if (setResult != 0) {
        LOG_ERROR("WebRTC") << "选择听筒设备"
                            << (useDefault ? QStringLiteral("「跟随系统默认」")
                                           : QString::number(index) + QStringLiteral(":"))
                            << "失败(返回" << setResult << ")";
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

    // 把解析结果一起记下：选错设备是静默故障，事后只能靠名字判断声音进了哪儿。
    const QString name = deviceNameAt(m_voiceAdm.get(), false,
                                      useDefault ? defaultDeviceQueryIndex()
                                                 : static_cast<uint16_t>(index));
    LOG_DEBUG("WebRTC") << "听筒已切到"
                       << (useDefault ? QStringLiteral("跟随系统默认:")
                                      : QString::number(index) + QStringLiteral(":"))
                       << (name.isEmpty() ? QStringLiteral("(取不到设备名)") : name)
                       << (wasPlaying ? "(热切换)" : "");
    return true;
}

void WebrtcManager::reloadDevices() {
    std::vector<DeviceInfo> recording;
    std::vector<DeviceInfo> playout;

    // 枚举要在 worker 线程上做（ADM 的线程约定）,结果拷回来在主线程存下来 ——
    // 这样 m_recordingDevices / m_playoutDevices 只有主线程碰，不用加锁。
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
    if (!name.isEmpty() && index == kDefaultDevice) {
        LOG_WARN("WebRTC") << (recording ? "麦克风" : "听筒") << "设备\"" << name
                           << "\"不在设备表里,退回系统默认";
    }

    // 目标就是现状，不用动 —— 免得白停一次播放/录音
    if (!force && index == currentDeviceIndex(recording))
        return true;

    bool ok = false;
    runOnVoiceWorker([&] {
        ok = recording ? applyRecordingDeviceOnWorker(index)
                       : applyPlayoutDeviceOnWorker(index);
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

    // 语音那一路：落到 ADM 上
    const bool applied = applyDevice(recording, name, /*force=*/false);

    if (changed) {
        // 听筒还有第二处出声的地方 —— 电影声（MovieAudioPlayer 的 QAudioSink）。
        // 它不受 WebRTC 引擎初始化影响，所以和 ADM 那边分开处理；
        // 只在真的变了的时候动它，否则每次启动都会白重启一次音频输出
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

    // 上次选的设备要是不在了（拔了）,明确回到"跟随系统默认" ——
    // 否则界面上一个都不高亮，看起来像坏了。顺带把还在用它的 ADM 一起退回去。
    for (const bool recording : {true, false}) {
        const QString desired = recording ? m_desiredRecordingName : m_desiredPlayoutName;
        if (desired.isEmpty())
            continue;
        if (indexOfDeviceName(desired, deviceList(recording)) != kDefaultDevice)
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
        // 没有视频轨（初始化失败）时不用报错：调用方是"按模式门控"的逻辑，
        // 拿不到轨就是没得发，静默即可
        return;
    }

    if (m_localVideoTrack->enabled() == enabled)
        return;

    m_localVideoTrack->set_enabled(enabled);

    // 切模式时才会走这里，频率很低，放 Info
    LOG_INFO("WebRTC") << (enabled ? "推送视频已开启" : "推送视频已关闭")
                       << "(当前" << m_peers.size() << "个对端)";
}

void WebrtcManager::setLocalVideoSending(bool enabled) {
    if (m_videoSending == enabled)
        return;

    m_videoSending = enabled;

    // 每个对端各有一条 media 连接，也就各有一个视频 sender
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        if (it.value().media != nullptr)
            it.value().media->applyVideoSending(enabled);
    }

    LOG_INFO("WebRTC") << (enabled ? "视频恢复发送" : "视频停止发送")
                       << "(当前" << m_peers.size() << "个对端)";
}

void WebrtcManager::setVideoQuality(VideoQuality quality) {
    if (m_videoQuality == quality && !m_peers.isEmpty())
        return;

    m_videoQuality = quality;
    if (m_videoSource != nullptr)
        m_videoSource->setMaxEncodeHeight(presetForQuality(quality).targetHeight);
    applyQualityToAll();

    const VideoQualityPreset preset = presetForQuality(quality);
    LOG_INFO("WebRTC") << "推送画质设为" << qualityName(quality)
                       << "(码率上限" << (preset.maxBitrateBps / 1000) << "kbps)"
                       << "目标高度" << preset.targetHeight << "p";
    emit videoQualityChanged(quality);
}

void WebrtcManager::applyQualityToLink(PeerLink *link) {
    if (link == nullptr)
        return;

    const VideoQualityPreset preset = presetForQuality(m_videoQuality);
    // 发送端转换阶段也按同一目标缩小；Original(0) 保持源分辨率。
    if (m_videoSource != nullptr)
        m_videoSource->setMaxEncodeHeight(preset.targetHeight);
    int maxBitrate = preset.maxBitrateBps;
    // 文件传输期间给视频封顶；传输结束后 setTransferActive(false) 会恢复。
    if (m_transferActive)
        maxBitrate = maxBitrate > 0 ? qMin(maxBitrate, 1'200'000) : 1'200'000;

    int maxWidth = 0;
    int maxHeight = 0;
    if (preset.targetHeight > 0) {
        int sourceWidth = m_videoSource != nullptr ? m_videoSource->inputWidth() : 0;
        int sourceHeight = m_videoSource != nullptr ? m_videoSource->inputHeight() : 0;
        // worker 还没更新统计时，用 GUI 最近一帧的尺寸兜底。
        if (sourceWidth <= 0 || sourceHeight <= 0) {
            sourceWidth = m_lastQualityWidth;
            sourceHeight = m_lastQualityHeight;
        }
        if (sourceWidth > 0 && sourceHeight > 0) {
            const bool portrait = sourceHeight > sourceWidth;
            const int longSide = preset.targetHeight * 16 / 9;
            maxWidth = portrait ? preset.targetHeight : longSide;
            maxHeight = portrait ? longSide : preset.targetHeight;
        }
    }

    link->applyVideoQuality(maxBitrate, preset.maxFramerate, maxWidth, maxHeight);
}

void WebrtcManager::applyQualityToAll() {
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        if (it.value().media != nullptr)
            applyQualityToLink(it.value().media);
    }
}

void WebrtcManager::setTransferActive(bool active) {
    if (m_transferActive == active)
        return;

    m_transferActive = active;
    applyQualityToAll();

    LOG_INFO("WebRTC") << (active
                               ? "文件传输进行中，共享视频码率上限降至 1.2Mbps"
                               : "文件传输结束，恢复共享视频画质档位");
}

void WebrtcManager::setMovieAudioEnabled(bool enabled) {
    if (!getOrCreateMovieAudioTrack()) {
        emit movieAudioEnabledChanged(false);
        return;
    }

    if (m_localMovieAudioTrack->enabled() == enabled)
        return;

    m_localMovieAudioTrack->set_enabled(enabled);

    // 这条跟着播放/暂停走，频率不低，放 Debug
    LOG_DEBUG("WebRTC") << (enabled ? "电影音轨已开启" : "电影音轨已关闭")
                        << "(当前" << m_peers.size() << "个对端)";
    emit movieAudioEnabledChanged(enabled);
}

void WebrtcManager::setMovieAudioSending(bool enabled) {
    if (m_movieAudioSending == enabled)
        return;

    m_movieAudioSending = enabled;

    // 每个对端各有一条 media 连接，也就各有一个电影音轨 sender
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        if (it.value().media != nullptr)
            it.value().media->applyMovieAudioSending(enabled);
    }

    LOG_DEBUG("WebRTC") << (enabled ? "电影音轨恢复发送" : "电影音轨停止发送")
                        << "(当前" << m_peers.size() << "个对端)";
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

    // 电影声与 WebRTC 无关，由 MovieAudioPlayer 控制
    if (m_movieAudioPlayer != nullptr)
        m_movieAudioPlayer->setVolume(volume);

    emit remoteMovieVolumeChanged(volume);
}

void WebrtcManager::setRemoteChatVolume(double volume) {
    volume = qBound(0.0, volume, 1.0);

    if (qFuzzyIsNull(m_remoteChatVolume - volume))
        return;

    m_remoteChatVolume = volume;

    // 语音音量由 WebRTC 自己的播放控制，所以要遍历所有对端连接，把 volume 传给它们的 PeerLink
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

    // 第一帧/换源后才能知道横竖屏与真实尺寸；此时把目标分辨率补套一次。
    const int width = frame.width();
    const int height = frame.height();
    if (width > 0 && height > 0
        && (width != m_lastQualityWidth || height != m_lastQualityHeight)) {
        m_lastQualityWidth = width;
        m_lastQualityHeight = height;
        applyQualityToAll();
    }

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

void WebrtcManager::flushRemotePlayback() {
    if (m_movieAudioPlayer != nullptr)
        m_movieAudioPlayer->flush();
    if (m_remoteRenderer != nullptr)
        m_remoteRenderer->resetQueue();
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

    // 真声卡：default_adm 传 nullptr,让 WebRTC 用平台默认设备。
    // 回声消除、自动增益、噪声抑制都靠它，语音质量全在这里。
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
    applyFactoryNetworkOptions(m_voiceFactory);

    // ---- 保活连接（声卡不被关闭的唯一理由）----
    // WebRTC 的 media engine（持有本端真声卡）按引用计数关闭，引用者只有
    // PeerConnection（pc/connection_context.cc 的 MediaEngineReference）。
    // 离开房间销毁全部对端 → 引用归零 → WebRtcVoiceEngine::Terminate() →
    // adm()->Terminate()。下次进房引擎会重新 Init，但 PulseAudio 分支上该操作
    // 不可逆（audio_device_pulse_linux.cc）：Terminate() 置 quit_ = true，
    // 仅构造函数会将其复位，Init() 重启的播放/录音线程读到 quit_ 后立即退出，
    // 导致 StartPlayout()/StartRecording() 等待 10 秒后返回 -1——
    // 第二次进房后麦克风与听筒全部失效（实测）。
    // 故保留一条不参与任何操作的连接，使引用计数恒 ≥ 1。
    // 这是上游缺陷的补丁，不应被清理；media 工厂不需要（假声卡无线程）。
    webrtc::PeerConnectionInterface::RTCConfiguration keepAliveConfig;
    webrtc::PeerConnectionDependencies keepAliveDeps(&m_voiceKeepAliveObserver);
    auto keepAliveResult =
        m_voiceFactory->CreatePeerConnectionOrError(keepAliveConfig, std::move(keepAliveDeps));
    if (keepAliveResult.ok()) {
        m_voiceKeepAlive = keepAliveResult.value();
        LOG_INFO("WebRTC") << "已建立常驻保活连接(防止退房时声卡被永久关掉)";
    } else {
        // 没建成也不算致命：只是"离开房间后再进房会没声音"那个老问题会回来
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
    applyFactoryNetworkOptions(m_mediaFactory);
    return true;
}

void WebrtcManager::destroy() {
    // 释放顺序有依赖，不可随意调整：对端 → 轨/源 → 工厂 → 线程
    // 理由：
    //   - 对端要在最前：关连接时 WebRTC 还可能在回调，线程必须还活着
    //   - 轨/音频源要在工厂之前：它们内部持有工厂引用，
    //     工厂先没了它们就指向一块已释放的内存
    //   - 轨要在源之前：轨引用源，反过来则会留下悬空引用
    //   - 工厂要在线程之前：工厂内部引用这些线程，
    //     先停线程会让工厂里的引用失效

    // 1. 对端连接 —— 必须在停线程之前
    closeAllPeers();

    // 保活连接也是连接，和解房间里的对端一样要放在工厂之前释放。
    // 走到这一步说明整个 WebRTC 世界都在拆，不需要它再撑着声卡了。
    m_voiceKeepAlive = nullptr;

    // removePeer 用的是 deleteLater；主事件循环退出后它排队的析构不会再执行，
    // 而 ~PeerLink 释放轨道时必须让 WebRTC 信令线程还活着。这里主动把
    // DeferredDelete 事件处理掉，确保 PeerLink 在停线程之前完成析构。
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    // 接收侧：渲染器的排期回调会读播放器。先清回调，再销毁渲染器
    // （停掉它的定时器与队列），最后才放播放器 —— 顺序反了就是悬空回调。
    if (m_remoteRenderer != nullptr)
        m_remoteRenderer->setPlayoutDelayProvider({});
    m_remoteRenderer.reset();

    // 播放器内部持有 WebRTC 的 sink 注册关系，必须在工厂之前释放
    m_movieAudioPlayer.reset();

    // 2. 轨 → 源 → 工厂（顺序同上：引用方先释放）
    m_localVideoTrack = nullptr;
    m_localAudioTrack = nullptr;
    m_localMovieAudioTrack = nullptr;
    // 源不会被 Release() 析构（永远 kOtherRefsRemained），worker 线程必须显式停。
    if (m_videoSource != nullptr)
        m_videoSource->shutdown();
    m_videoSource = nullptr;
    m_audioSource = nullptr;
    m_movieAudioSource = nullptr;

    m_voiceFactory = nullptr;
    m_mediaFactory = nullptr;

    // 3. 线程 —— 必须在工厂之后
    stopThreadSet(m_voiceNetworkThread, m_voiceWorkerThread, m_voiceSignalingThread);
    stopThreadSet(m_mediaNetworkThread, m_mediaWorkerThread, m_mediaSignalingThread);

    // 4. 两块声卡和环境放最后：工厂已经不再持有它们了。
    //    **ADM 须早于 Environment 释放** —— 它们内部持有对环境的引用
    //    （Environment 为值类型、各自存有一份，但顺序颠倒仍会导致错误）。
    m_dummyAdm = nullptr;
    m_voiceAdm = nullptr;
    m_mediaEnvironment.reset();

    m_initialized = false;
}
