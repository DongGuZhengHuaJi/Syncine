//
// 共享模式开关的发送侧内存探针（诊断工具，不是单元测试）。
//
// 复现「房主进出共享模式后 RSS 永久抬升」：在本进程内建两条真实 PeerLink
// 并通过回环连通，用真实的 VideoTrackSource 推 4K 帧，然后按 main.cpp
// 的调用顺序反复停发/恢复：
//     stop : track->set_enabled(false) + PeerLink::applyVideoSending(false)
//     start: track->set_enabled(true)  + PeerLink::applyVideoSending(true)
// 接收端用空解码器（只收包不解码），把 RSS 变化归因到发送侧编码器。
//
// 用法（需要显式开启，CI 默认跳过）：
//   SYNCINE_SHARE_MEMORY_PROBE=1 ./syncine_share_memory_probe
// 可调：
//   SYNCINE_SHARE_PROBE_VIDEO   指向一段真实视频：改用 QMediaPlayer 解码喂帧，
//                               复现“共享模式播放 → 从播放列表移除该条目”的
//                               完整路径（含解码器重建）；不设则用合成 4K 帧
//   SYNCINE_SHARE_PROBE_ROUNDS  停止/恢复轮数（默认 6）
//   SYNCINE_SHARE_PROBE_HEIGHT  编码目标短边（默认 0 = 原分辨率 4K；1080 = 默认档位）
//   SYNCINE_SHARE_PROBE_FPS     推帧帧率（默认 25）
//   SYNCINE_SHARE_PROBE_HOLD_MS 每轮保持推流的时长（默认 1500）
//   SYNCINE_SHARE_PROBE_MODE    both | active | enabled（默认 both，只切换其中一项来定位）
//   SYNCINE_SHARE_PROBE_TRIM    1 = 每轮停发后调用 malloc_trim(0)，看是不是分配器滞留
//
// 判定只打印，不硬失败：这里的目标是给出可比较的数字，是否收敛由人看。
//

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "api/video_codecs/builtin_video_encoder_factory.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_decoder.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "rtc_base/thread.h"

#include "playback/PlaybackController.h"
#include "webrtc/PeerLink.h"
#include "webrtc/VideoTrackSource.h"

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

qint64 rssKb()
{
#if defined(__linux__)
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (!statm.open(QIODevice::ReadOnly))
        return -1;
    const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
    if (fields.size() < 2)
        return -1;
    bool ok = false;
    const qint64 pages = fields.at(1).toLongLong(&ok);
    if (!ok)
        return -1;
    return pages * (qint64(sysconf(_SC_PAGESIZE)) / 1024);
#else
    return -1;
#endif
}

QString mb(qint64 kb)
{
    if (kb < 0)
        return QStringLiteral("?");
    return QString::number(kb / 1024.0, 'f', 1);
}

bool waitFor(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

// 只收包、不解码的接收端：把 RSS 变化全部归到发送侧（编码器）。
class NullVideoDecoder : public webrtc::VideoDecoder {
public:
    bool Configure(const Settings &) override { return true; }
    int32_t Decode(const webrtc::EncodedImage &, int64_t) override { return 0; }
    int32_t RegisterDecodeCompleteCallback(webrtc::DecodedImageCallback *) override {
        return 0;
    }
    int32_t Release() override { return 0; }
    DecoderInfo GetDecoderInfo() const override {
        return DecoderInfo{QStringLiteral("NullVideoDecoder").toStdString(), false};
    }
};

class NullVideoDecoderFactory : public webrtc::VideoDecoderFactory {
public:
    std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
        // 用编码器支持的格式做答案侧的能力表，保证协商能对上
        return webrtc::CreateBuiltinVideoEncoderFactory()->GetSupportedFormats();
    }

    std::unique_ptr<webrtc::VideoDecoder> Create(
        const webrtc::Environment &, const webrtc::SdpVideoFormat &) override {
        return std::make_unique<NullVideoDecoder>();
    }
};

QVideoFrame makeYuvFrame(int width, int height)
{
    QVideoFrameFormat format(QSize(width, height),
                             QVideoFrameFormat::Format_YUV420P);
    QVideoFrame frame(format);
    if (!frame.map(QVideoFrame::WriteOnly))
        return {};

    for (int plane = 0; plane < 3; ++plane) {
        uchar *bits = frame.bits(plane);
        const int stride = frame.bytesPerLine(plane);
        const int rows = plane == 0 ? height : (height + 1) / 2;
        const int cols = plane == 0 ? width : (width + 1) / 2;
        for (int row = 0; row < rows; ++row)
            std::memset(bits + static_cast<ptrdiff_t>(row) * stride,
                        plane == 0 ? 16 : 128, static_cast<size_t>(cols));
    }
    frame.unmap();
    return frame;
}

} // namespace

int main(int argc, char **argv)
{
    if (!qEnvironmentVariableIntValue("SYNCINE_SHARE_MEMORY_PROBE")) {
        std::printf("share-memory: 未设置 SYNCINE_SHARE_MEMORY_PROBE=1，跳过\n");
        return 0;
    }
    if (rssKb() < 0) {
        std::printf("share-memory: 当前平台无法读取 RSS，跳过\n");
        return 0;
    }

    QCoreApplication app(argc, argv);

    bool roundsOk = false;
    const int roundsEnv =
        qEnvironmentVariableIntValue("SYNCINE_SHARE_PROBE_ROUNDS", &roundsOk);
    const int rounds = qMax(1, roundsOk ? roundsEnv : 6);
    const int targetHeight =
        qEnvironmentVariableIntValue("SYNCINE_SHARE_PROBE_HEIGHT"); // 缺省 0 = 原画
    bool fpsOk = false;
    const int fpsEnv =
        qEnvironmentVariableIntValue("SYNCINE_SHARE_PROBE_FPS", &fpsOk);
    const int fps = qMax(1, fpsOk ? fpsEnv : 25);
    bool holdOk = false;
    const int holdMsEnv =
        qEnvironmentVariableIntValue("SYNCINE_SHARE_PROBE_HOLD_MS", &holdOk);
    const int holdMs = qMax(200, holdOk ? holdMsEnv : 1500);
    const QString mode = qEnvironmentVariable("SYNCINE_SHARE_PROBE_MODE",
                                              "both");
    const bool trim = qEnvironmentVariableIntValue("SYNCINE_SHARE_PROBE_TRIM") != 0;

    // 可选：用真实播放器解码一段视频来喂帧，复现“共享模式播放 → 从播放列表
    // 移除该条目”的完整路径（解码 + 推流编码在同一进程）。不设则用合成 4K 帧。
    const QString videoPath = qEnvironmentVariable("SYNCINE_SHARE_PROBE_VIDEO");
    const bool playerMode = !videoPath.isEmpty();
    if (playerMode && !QFile::exists(videoPath)) {
        std::fprintf(stderr, "SYNCINE_SHARE_PROBE_VIDEO 指向的文件不存在: %s\n",
                     qPrintable(videoPath));
        return 1;
    }

    // ---- WebRTC 线程与工厂（与 WebrtcManager::initializeMediaFactory 对齐）----
    webrtc::Environment env = webrtc::CreateEnvironment();
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm =
        webrtc::CreateAudioDeviceModule(env,
                                        webrtc::AudioDeviceModule::kDummyAudio);
    std::unique_ptr<webrtc::Thread> network = webrtc::Thread::CreateWithSocketServer();
    std::unique_ptr<webrtc::Thread> worker = webrtc::Thread::Create();
    std::unique_ptr<webrtc::Thread> signaling = webrtc::Thread::Create();
    network->Start();
    worker->Start();
    signaling->Start();

    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory =
        webrtc::CreatePeerConnectionFactory(
            network.get(), worker.get(), signaling.get(), adm,
            webrtc::CreateBuiltinAudioEncoderFactory(),
            webrtc::CreateBuiltinAudioDecoderFactory(),
            webrtc::CreateBuiltinVideoEncoderFactory(),
            std::make_unique<NullVideoDecoderFactory>(),
            /*audio_mixer=*/nullptr, /*audio_processing=*/nullptr);
    if (factory == nullptr) {
        std::fprintf(stderr, "创建 PeerConnectionFactory 失败\n");
        return 1;
    }

    // ---- 本地视频源与轨（与 PlaybackController/WebrtcManager 的接线一致）----
    auto source = webrtc::scoped_refptr<VideoTrackSource>(new VideoTrackSource());
    source->setMaxEncodeHeight(targetHeight);
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> track =
        factory->CreateVideoTrack(source, "syncine-video");
    CHECK(track != nullptr);

    // ---- 两条回环连接：a 推流，b 收包（不解码）----
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    PeerLink a(QStringLiteral("loop-a"), LinkKind::Media, factory, config, false);
    PeerLink b(QStringLiteral("loop-b"), LinkKind::Media, factory, config, false);
    CHECK(a.isValid() && b.isValid());

    bool aConnected = false;
    bool bConnected = false;
    QObject::connect(&a, &PeerLink::connected, &app,
                     [&aConnected] { aConnected = true; });
    QObject::connect(&b, &PeerLink::connected, &app,
                     [&bConnected] { bConnected = true; });
    QObject::connect(&a, &PeerLink::offerCreated, &app,
                     [&b](const QString &, const QString &, const QString &sdp) {
                         b.setRemoteDescription(sdp, QStringLiteral("offer"));
                     });
    QObject::connect(&b, &PeerLink::answerCreated, &app,
                     [&a](const QString &, const QString &, const QString &sdp) {
                         a.setRemoteDescription(sdp, QStringLiteral("answer"));
                     });
    QObject::connect(&a, &PeerLink::errorOccurred, &app,
                     [](const QString &p, const QString &l, const QString &m) {
                         std::fprintf(stderr, "a error %s/%s: %s\n",
                                      qPrintable(p), qPrintable(l), qPrintable(m));
                     });
    QObject::connect(&b, &PeerLink::errorOccurred, &app,
                     [](const QString &p, const QString &l, const QString &m) {
                         std::fprintf(stderr, "b error %s/%s: %s\n",
                                      qPrintable(p), qPrintable(l), qPrintable(m));
                     });

    const auto relayCandidates = [&app](PeerLink &from, PeerLink &to) {
        QObject::connect(&from, &PeerLink::iceCandidateCreated, &app,
                         [&to](const QString &, const QString &, const QString &sdp,
                               const QString &mid, int index) {
                             to.addIceCandidate(sdp, mid, index);
                         });
    };
    relayCandidates(a, b);
    relayCandidates(b, a);

    a.addLocalVideoTrack(track, /*sending=*/true);
    a.createOffer();

    if (!waitFor([&] { return aConnected && bConnected; }, 15000)) {
        std::fprintf(stderr, "回环连接未建立（a=%d b=%d）\n",
                     aConnected ? 1 : 0, bConnected ? 1 : 0);
        return 1;
    }
    std::printf("share-memory: 回环已连接，源=%s，编码目标高度=%s，%d 轮，模式=%s，trim=%d\n",
                playerMode ? qPrintable(QFileInfo(videoPath).fileName())
                           : "合成4K",
                targetHeight > 0 ? qPrintable(QString::number(targetHeight))
                                 : "原画",
                rounds, qPrintable(mode), trim ? 1 : 0);

    // ---- 喂帧 ----
    // 合成帧与真实解码帧都只在"共享中"喂：main.cpp 的发送接线按房间模式門控，
    // 切走后不再喂帧。探针必须同样门控，否则停发期间的 4K 转换开销会把
    // 编码器本身的变化淹掉。
    bool sharing = false;
    int playerFrames = 0;

    const QVideoFrame frame = makeYuvFrame(3840, 2160);
    CHECK(frame.isValid());
    QTimer pushTimer;
    pushTimer.setInterval(qMax(1, 1000 / fps));
    QObject::connect(&pushTimer, &QTimer::timeout, &app,
                     [&source, &frame, &sharing] {
                         if (sharing)
                             source->pushQtFrame(frame);
                     });

    // 播放器模式：真实 QMediaPlayer 解码 → PlaybackController 的帧信号
    // → VideoTrackSource，与 main.cpp 的接线相同。
    QVideoSink sink;
    std::unique_ptr<PlaybackController> controller;
    if (playerMode) {
        controller = std::make_unique<PlaybackController>();
        controller->bindVideoOutput(&sink);
        QObject::connect(controller.get(), &PlaybackController::videoFrameAvailable,
                         &app, [&source, &sharing, &playerFrames](const QVideoFrame &f) {
                             if (!sharing)
                                 return;
                             ++playerFrames;
                             source->pushQtFrame(f);
                         });
    } else {
        pushTimer.start();
    }

    const bool toggleActive = mode != QLatin1String("enabled");
    const bool toggleEnabled = mode != QLatin1String("active");

    std::printf("%4s %12s %12s %12s %8s\n", "轮", "共享中MB", "停发后MB",
                "trim后MB", "订阅数");
    std::vector<qint64> stoppedRss;

    for (int round = 1; round <= rounds; ++round) {
        // 恢复共享（main.cpp 的 updateVideoEnabled(true) + 发送接线恢复）。
        // 播放器模式额外重新加载并播放：等价于重新把视频加进播放列表。
        sharing = true;
        if (playerMode) {
            playerFrames = 0;
            controller->load(QUrl::fromLocalFile(videoPath));
            controller->play();
        } else {
            if (toggleEnabled)
                track->set_enabled(true);
            if (toggleActive)
                a.applyVideoSending(true);
        }

        waitFor([] { return false; }, holdMs);
        if (playerMode)
            CHECK(playerFrames > 0);

        const qint64 loaded = rssKb();

        // 停止共享 / 从播放列表移除该条目。
        sharing = false;
        if (playerMode) {
            // 与 PlaybackSync::onPlaylistItemRemoved 一致：卸载播放器
            // （内部会清最后一帧并 malloc_trim），推流侧不动。
            controller->unload();
        } else {
            if (toggleEnabled)
                track->set_enabled(false);
            if (toggleActive)
                a.applyVideoSending(false);
        }

        waitFor([] { return false; }, 500);

        const qint64 stopped = rssKb();
        qint64 trimmed = -1;
#if defined(__GLIBC__)
        if (trim) {
            malloc_trim(0);
            trimmed = rssKb();
        }
#endif
        stoppedRss.push_back(stopped);

        std::printf("%4d %12s %12s %12s %8d\n", round,
                    qPrintable(mb(loaded)),
                    qPrintable(mb(stopped)),
                    trim ? qPrintable(mb(trimmed)) : "-",
                    source->sinkCount());
    }

    pushTimer.stop();

    if (stoppedRss.size() >= 2) {
        const qint64 first = stoppedRss.front();
        const qint64 last = stoppedRss.back();
        std::printf("share-memory: 停发后 RSS 首轮 %.1f MB，末轮 %.1f MB，"
                    "累计变化 %+.1f MB（%.1f MB/轮）\n",
                    first / 1024.0, last / 1024.0,
                    (last - first) / 1024.0,
                    (last - first) / 1024.0 / (stoppedRss.size() - 1));
    }

    // 收尾：先松轨，再停源 worker，最后让连接/工厂/线程按声明逆序析构
    track = nullptr;
    source->shutdown();

    return g_failures == 0 ? 0 : 1;
}
