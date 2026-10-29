//
// Created by donggu on 2026/9/6.
//

#include "PlaybackController.h"
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QUrl>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QVideoFrame>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

// 只为了转发"远端渲染器该往哪个 sink 写"这一条信息。
// 本类不知道 WebRTC 的任何细节 —— 那是 WebrtcManager 的事。
#include "webrtc/WebrtcManager.h"

#include "core/Log.h"
#include "core/MediaHash.h"

PlaybackController::PlaybackController(QObject *parent)
    : QObject(parent)
{
    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);

    // 将音频输出绑定到播放器
    m_player->setAudioOutput(m_audioOutput);

    // ---- 电影音频的旁路输出 ----
    // QAudioBufferOutput（Qt 6.8 引入）：播放器解码的同时旁路输出一份 PCM。
    // **构造函数中传入的格式即产出格式**——Qt 据此建立重采样器，故直接把格式定为
    // WebRTC 所需的，重采样由 Qt 顺带完成（见 qffmpegaudiorenderer.cpp）。
    // 48000Hz 为 WebRTC Opus 的原生采样率，其他采样率反而需它自行转换。
    // 双声道需配合 media 工厂中 Opus 格式的 stereo=1 参数（见 WebrtcManager.cpp 的
    // StereoOpusAudioEncoder/DecoderFactory，两者都要包装，否则会被下混为单声道）。
    QAudioFormat movieAudioFormat;
    movieAudioFormat.setSampleRate(48000);
    movieAudioFormat.setChannelCount(2);
    movieAudioFormat.setSampleFormat(QAudioFormat::Int16);

    m_audioBufferOutput = new QAudioBufferOutput(movieAudioFormat, this);
    m_player->setAudioBufferOutput(m_audioBufferOutput);

    connect(m_audioBufferOutput, &QAudioBufferOutput::audioBufferReceived,
            this, &PlaybackController::onMovieAudioBuffer);

    // 播放状态发生变化
    connect(
        m_player,
        &QMediaPlayer::playbackStateChanged,
        this,
        [this](QMediaPlayer::PlaybackState) {
            emit playingChanged();
        }
    );

    // 可跳转状态发生变化
    connect(
        m_player,
        &QMediaPlayer::seekableChanged,
        this,
        [this](bool) {
            emit seekableChanged();
        }
    );

    // 播放位置发生变化
    connect(
        m_player,
        &QMediaPlayer::positionChanged,
        this,
        [this](qint64) {
            emit positionChanged();
        }
    );

    // 视频总时长发生变化
    connect(
        m_player,
        &QMediaPlayer::durationChanged,
        this,
        [this](qint64) {
            emit durationChanged();
        }
    );

    // 倍速变化。Qt 可能在引擎重建后重新应用 setPlaybackRate，
    // 所以统一从这里转发，而不是在 setter 里手动发信号。
    connect(
        m_player,
        &QMediaPlayer::playbackRateChanged,
        this,
        [this](qreal rate) {
            emit playbackRateChanged(rate);
        }
    );

    // 播放器错误
    connect(
        m_player,
        &QMediaPlayer::errorOccurred,
        this,
        [this](QMediaPlayer::Error,
               const QString &errorString) {

            LOG_WARN("Playback") << "播放器报错:" << errorString;
            emit errorOccurred(errorString);
        }
    );

    // 播放状态变化。**网络流（尤其是直播）卡顿只有从这里看得出来**:
    // StalledMedia = 等数据等停了（网络的锅）,BufferingMedia = 在重新缓冲。
    // 没有这条日志的话，"卡了"只能靠猜。放 Debug:正常播放时它很安静，
    // 只在状态真的变时才打一行。
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                const char *text = nullptr;
                switch (status) {
                case QMediaPlayer::NoMedia: text = "无媒体"; break;
                case QMediaPlayer::LoadingMedia: text = "加载中"; break;
                case QMediaPlayer::LoadedMedia: text = "已加载"; break;
                case QMediaPlayer::StalledMedia: text = "卡住了(等数据)"; break;
                case QMediaPlayer::BufferingMedia: text = "缓冲中"; break;
                case QMediaPlayer::BufferedMedia: text = "缓冲够用"; break;
                case QMediaPlayer::EndOfMedia:
                    text = "播完了";
                    // 播完导致的 playingChanged(false) 不是用户暂停，不广播。
                    m_lifecycleStop = true;
                    break;
                case QMediaPlayer::InvalidMedia: text = "媒体无效"; break;
                }
                if (text != nullptr)
                    LOG_DEBUG("Playback") << "播放状态:" << text;
            });

    // 真正进入播放状态（用户点了播放）后，生命周期停止标记复位。
    connect(m_player, &QMediaPlayer::playbackStateChanged, this,
            [this](QMediaPlayer::PlaybackState state) {
                if (state == QMediaPlayer::PlayingState)
                    m_lifecycleStop = false;
            });
}


// ============================
// 状态
// ============================

bool PlaybackController::hasLoaded() const {
    return m_player->source().isValid();
}

bool PlaybackController::playing() const
{
    return m_player->playbackState()
           == QMediaPlayer::PlayingState;
}

bool PlaybackController::seekable() const
{
    return m_player->isSeekable();
}

qint64 PlaybackController::position() const
{
    return m_player->position();
}

qint64 PlaybackController::duration() const
{
    return m_player->duration();
}

qreal PlaybackController::playbackRate() const {
    return m_player->playbackRate();
}

void PlaybackController::setPlaybackRate(qreal playbackRate) {
    // 界面只给固定档位，这里兜底防止协议或其它调用方传进异常值。
    // 0 在 Qt 里意味着"完全停住"，不应作为倍速使用。
    playbackRate = qBound(0.25, playbackRate, 4.0);

    m_player->setPlaybackRate(playbackRate);
}


// ============================
// 音量
// ============================

double PlaybackController::volume() const
{
    return m_audioOutput->volume();
}

void PlaybackController::setVolume(double volume)
{
    // 限制在 0~1
    volume = qBound(0.0, volume, 1.0);

    if (qFuzzyCompare(static_cast<double>(m_audioOutput->volume()), volume))
        return;

    m_audioOutput->setVolume(static_cast<float>(volume));

    emit volumeChanged();
}

void PlaybackController::setOutputDevice(const QAudioDevice &device)
{
    if (device.id() == m_audioOutput->device().id())
        return;

    // QAudioOutput 自带 setDevice()(QAudioSink 没有，那边只能重建),
    // 播放中切换由 Qt 自己把底层输出重新接一遍，音量和静音状态不受影响。
    // 切记**不要**为了换设备去 setAudioOutput(nullptr) —— 那会把播放器的
    // 音频渲染器一起拆掉（见头文件里 m_audioBufferOutput 那段说明）。
    m_audioOutput->setDevice(device);

    LOG_INFO("Playback") << "本机播放输出设备已切换:"
                         << (device.description().isEmpty()
                                 ? QStringLiteral("(系统默认)")
                                 : device.description())
                         << "当前播放状态:" << playing();
}

// ============================
// 静音
// ============================

bool PlaybackController::muted() const
{
    return m_audioOutput->isMuted();
}

void PlaybackController::setMuted(bool muted)
{
    if (m_audioOutput->isMuted() == muted)
        return;

    m_audioOutput->setMuted(muted);

    emit mutedChanged();
}

QString PlaybackController::hash() const {
    if (!hasLoaded())
        return QString();

    const QUrl source = m_player->source();
    // 算法在 MediaHash 里，和播放列表上报 playlist_status 用的是同一份 ——
    // 两处不一致的话，"同不同步"的判断就会互相打架
    if (source.isLocalFile())
        return MediaHash::forFile(source.toLocalFile());

    return MediaHash::forText(source.toString());
}


// ============================
// 获取 QMediaPlayer
// ============================

QMediaPlayer *PlaybackController::player() const
{
    return m_player;
}

void PlaybackController::onVideoFrame(const QVideoFrame &frame) {
    // clearVideoFrame() 注入的是 2×2 黑帧；即使信号是排队投递的、标志已经复位，
    // 也用尺寸兜一道，避免它被当成真实解码帧转发给推流端。
    if (!frame.isValid() || m_injectingClearFrame
        || (frame.width() == 2 && frame.height() == 2))
        return;

    // 本回调接在 m_outputSink 上，共享模式下远端渲染器也会往这里送帧
    // 只有显示本地时才往外转发，否则会产生回声
    if (m_showingRemote)
        return;

    // 诊断：前 3 帧 + 每 300 帧打印一次
    static int count = 0;
    if (count < 3 || count % 300 == 0) {
        LOG_TRACE("Playback") << "onVideoFrame 第" << (count + 1) << "帧"
                              << frame.width() << "x" << frame.height();
    }
    ++count;

    // 向外转发，WebrtcManager捕获
    emit videoFrameAvailable(frame);
}

void PlaybackController::onMovieAudioBuffer(const QAudioBuffer &buffer) {
    if (!buffer.isValid())
        return;

    static int count = 0;

    // 首帧格式用 info 记一次。默认级别就是 info，而这条链路的两处格式约定
    // （QAudioBufferOutput 的构造格式、opus 协商出的声道数）一旦对不上，
    // 观众端就是全程静音 —— 这是事后唯一能证明"发送端产出了什么"的记录。
    if (count == 0) {
        const QAudioFormat format = buffer.format();
        LOG_INFO("Playback") << "电影音频首帧:" << format.sampleRate() << "Hz,"
                 << format.channelCount() << "声道,"
                 << format.sampleFormat();
    }

    // 诊断：音频帧比视频密得多（一帧几十毫秒）,全打会淹掉日志，
    // 所以只打开头几次 + 每隔一段。排障时先看这一行有没有出现 ——
    // 没有就说明"取 PCM"这一环没通，和 WebRTC 那边无关。
    if (count < 3 || count % 500 == 0) {
        const QAudioFormat format = buffer.format();

        // 峰值：用于区分「播放器输出的本就是静音」与「PCM 未被送出」。
        // 此处恒为 0 时，问题在播放器/取 PCM 环节（例如被静音）；
        // 此处非 0 而对端收到 0 时，问题在 MovieAudioSource/WebRTC 一段。
        int peak = 0;
        if (format.sampleFormat() == QAudioFormat::Int16) {
            const auto *samples = buffer.constData<int16_t>();
            const qsizetype total = buffer.sampleCount();
            for (qsizetype i = 0; i < total; ++i) {
                const int v = samples[i] < 0 ? -samples[i] : samples[i];
                if (v > peak)
                    peak = v;
            }
        }

        LOG_TRACE("Playback") << "电影音频第" << (count + 1) << "帧:"
                 << buffer.frameCount() << "采样/声道,"
                 << format.sampleRate() << "Hz,"
                 << format.channelCount() << "声道,"
                 << format.sampleFormat() << ", 峰值" << peak;
    }
    ++count;

    // Qt根据m_audioBufferOutput的构造参数重采样，这里的格式为 48k/双声道/Int16。
    emit movieAudioFrameAvailable(buffer);
}

QVideoSink *PlaybackController::displayVideoSink() const {
    return m_outputSink.data();
}

QUrl PlaybackController::source() const {
    return m_player != nullptr ? m_player->source() : QUrl();
}

bool PlaybackController::showingRemote() const {
    return m_showingRemote;
}

void PlaybackController::bindVideoOutput(QVideoSink *sink) {
    if (sink == nullptr)
        return;

    if (m_outputSink == sink)
        return;                         // 已经绑过了

    // 重新绑定时断开旧 sink，避免同一个对象上留下两条连接。
    if (!m_outputSink.isNull())
        disconnect(m_outputSink.data(), nullptr, this, nullptr);

    m_outputSink = sink;

    // 本地解码帧到达时往外发一份 —— 推流侧订阅这个信号。
    connect(sink, &QVideoSink::videoFrameChanged,
            this, &PlaybackController::onVideoFrame);

    // 向sink送帧的对象可能是播放器也可能是远端渲染器，取决于显示本地还是远端。
    applyVideoSink();

    // 将sink交给 WebrtcManager,让远端渲染器往这里送帧。
    if (m_webrtcManager != nullptr)
        m_webrtcManager->setRemoteVideoSink(sink);

    LOG_DEBUG("Playback") << "视频输出端已绑定:" << sink;
    emit displayVideoSinkChanged();
}

void PlaybackController::unbindVideoOutput() {
    if (m_outputSink.isNull())
        return;

    // QML 的 VideoOutput 即将销毁：先清掉最后一帧，再断开它的信号，
    // 最后把播放器/渲染器两边的落点清空并松开 QPointer。
    // 否则 sink 在真正销毁前仍会持有最后一帧（4K 下可达十几 MB，另加纹理）。
    clearVideoFrame();
    disconnect(m_outputSink.data(), nullptr, this, nullptr);
    if (m_player != nullptr)
        m_player->setVideoSink(nullptr);
    if (m_webrtcManager != nullptr)
        m_webrtcManager->setRemoteVideoSink(nullptr);

    m_outputSink = nullptr;
    emit displayVideoSinkChanged();
}

void PlaybackController::setRemoteRenderer(WebrtcManager *manager) {
    m_webrtcManager = manager;

    // 如果 sink 已经交过来了（顺序反过来）,立刻接上
    if (m_webrtcManager != nullptr && m_outputSink != nullptr)
        m_webrtcManager->setRemoteVideoSink(m_outputSink.data());
}

void PlaybackController::setShowingRemote(bool showing) {
    if (m_showingRemote == showing)
        return;

    m_showingRemote = showing;


    // 显示本地 → 播放器往 sink 送（远端渲染器停手）
    // 显示远端 → 远端渲染器往 sink 送（播放器停手）
    applyVideoSink();

    emit showingRemoteChanged();
}

void PlaybackController::applyVideoSink() {
    // bindVideoOutput 还没交过来 sink
    if (m_outputSink == nullptr)
        return;

    if (m_showingRemote) {
        // 远端画面由 WebrtcManager 的渲染器直接推进 m_outputSink。
        m_player->setVideoSink(nullptr);

        // 渲染器开始往 sink 写帧
        if (m_webrtcManager != nullptr)
            m_webrtcManager->setRemoteVideoEnabled(true);

        // 先清掉切换前本地画面的最后一帧；远端首帧到达前显示黑屏，
        // 否则会短暂停留在上一路画面上。
        clearVideoFrame();

        LOG_DEBUG("Playback") << "applyVideoSink: 显示远端,播放器已脱钩";
    } else {
        // 显示本地画面，播放器往 sink 送帧
        m_player->setVideoSink(m_outputSink.data());

        // 停止远端渲染器
        if (m_webrtcManager != nullptr)
            m_webrtcManager->setRemoteVideoEnabled(false);

        // **必须主动清一帧**：VideoOutput 不会因为远端渲染器停手就自动变黑，
        // 房主切走共享模式后观众端会一直残留最后一帧。
        // 本地播放器有画面时，下一帧会立刻覆盖这帧黑色。
        clearVideoFrame();

        LOG_DEBUG("Playback") << "applyVideoSink: 显示本地,播放器 →" << m_outputSink.data()
                              << "实际生效:" << m_player->videoSink();
    }
}

void PlaybackController::clearVideoFrame() {
    if (m_outputSink == nullptr)
        return;

    // 用一帧 2×2 纯黑图像清屏。QVideoSink 没有“clear”接口，
    // 而 VideoOutput 自己不会在无人送帧时重绘黑色。
    QImage black(2, 2, QImage::Format_RGB32);
    black.fill(Qt::black);
    m_injectingClearFrame = true;
    m_outputSink->setVideoFrame(QVideoFrame(black));
    m_injectingClearFrame = false;
}



// ============================
// 加载视频
// ============================

void PlaybackController::load(const QUrl &source)
{
    if (!source.isValid())
        return;

    if (m_player->source() == source
        && m_player->mediaStatus() != QMediaPlayer::NoMedia
        && m_player->mediaStatus() != QMediaPlayer::InvalidMedia) {
        LOG_DEBUG("Playback") << "媒体源未变,忽略重复加载:" << source;
        return;
    }

    LOG_INFO("Playback") << "加载媒体:" << source;

    // 换源会让播放器 Stopped，这个 playingChanged(false) 不是用户暂停。
    m_lifecycleStop = true;
    m_player->setSource(source);
    emit sourceChanged();
    emit hasLoadedChanged();
}

void PlaybackController::unload() {
    m_lifecycleStop = true;
    // 只有确实卸载过一个源才值得整理 allocator；重复的空卸载（共享模式观众、
    // 空列表等路径会反复走到这里）直接跳过。
    const bool hadSource = m_player->source().isValid();

    m_player->stop();
    m_player->setSource(QUrl());

    // QMediaPlayer 置空后会释放解码器，但界面的 QVideoSink 仍会保留最后
    // 一帧。4K 下一帧就有十几 MB，删除条目/切网链模式时会在下载期间一直
    // 挂着。这里用 2×2 黑帧替换掉它；下一帧真实画面到达时会自然覆盖。
    clearVideoFrame();

#if defined(__GLIBC__)
    // 解码器与 FFmpeg 帧池释放后，内存大多留在 glibc 的 arena 里不归还系统。
    // 实测重复加载同一段 4K 视频：不 trim 时卸载后 RSS 在 430–600MB、解码峰值
    // 约 750MB；每次卸载 trim 后是 210–280MB / 510–535MB。卸载总耗时（主要是
    // 播放器自身拆卸，约 0.1 秒）加不加 trim 没有可测差别。
    if (hadSource)
        malloc_trim(0);
#endif

    emit sourceChanged();
    emit hasLoadedChanged();
}


// ============================
// 播放
// ============================

void PlaybackController::play()
{
    // 用户主动播放：此后的暂停才需要广播。
    m_lifecycleStop = false;
    m_player->play();
}


// ============================
// 暂停
// ============================

void PlaybackController::pause()
{
    m_player->pause();
}


// ============================
// 播放 / 暂停
// ============================

void PlaybackController::togglePlayPause()
{
    if (playing())
        pause();
    else
        play();
}


// ============================
// 跳转
// ============================

void PlaybackController::seek(qint64 position)
{
    if (duration() <= 0)
        return;

    position = qBound(
        qint64(0),
        position,
        duration()
    );

    m_player->setPosition(position);

    emit userSeeked(position);
}


// ============================
// 相对跳转
// ============================

void PlaybackController::seekRelative(qint64 offset)
{
    seek(position() + offset);
}


// ============================
// 静音切换
// ============================

void PlaybackController::toggleMute()
{
    setMuted(!muted());
}
