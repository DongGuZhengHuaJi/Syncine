//
// Created by donggu on 2026/9/6.
//

#include "PlaybackController.h"
#include <QCryptographicHash>
#include <QFile>
#include <QUrl>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QVideoSink>
#include <QVideoFrame>

// 只为了转发"远端渲染器该往哪个 sink 写"这一条信息。
// 本类不知道 WebRTC 的任何细节 —— 那是 WebrtcManager 的事。
#include "webrtc/WebrtcManager.h"
#include <QDebug>

PlaybackController::PlaybackController(QObject *parent)
    : QObject(parent)
{
    m_player = new QMediaPlayer(this);
    m_audioOutput = new QAudioOutput(this);

    // 将音频输出绑定到播放器
    m_player->setAudioOutput(m_audioOutput);

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

    // 播放器错误
    connect(
        m_player,
        &QMediaPlayer::errorOccurred,
        this,
        [this](QMediaPlayer::Error,
               const QString &errorString) {

            emit errorOccurred(errorString);
        }
    );
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
    QByteArray data;
    if (source.isLocalFile()) {
        // 内容哈希:512KB
        QFile file(source.toLocalFile());
        if (!file.open(QIODevice::ReadOnly))
            return QString();
        data = file.read(512 * 1024);
        data.append(QByteArray::number(file.size()));
    } else {
        // 非本地源暂以 URL 代替
        data = source.toString().toUtf8();
    }

    const QByteArray hashData =
        QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    return QString(hashData.toHex());
}


// ============================
// 获取 QMediaPlayer
// ============================

QMediaPlayer *PlaybackController::player() const
{
    return m_player;
}

void PlaybackController::onVideoFrame(const QVideoFrame &frame) {
    if (!frame.isValid())
        return;


    // 本回调接在 m_outputSink 上,共享模式下远端渲染器也会往这里送帧
    // 只有显示本地时才往外转发,否则会产生回声
    if (m_showingRemote)
        return;

    // 诊断:前 10 帧 + 每 300 帧打印一次
    static int count = 0;
    if (count < 10 || count % 300 == 0) {
        qDebug() << "[诊断] onVideoFrame 第" << (count + 1) << "帧"
                 << frame.width() << "x" << frame.height();
    }
    ++count;

    // 向外转发，WebrtcManager捕获
    emit videoFrameAvailable(frame);
}

QVideoSink *PlaybackController::displayVideoSink() const {
    return m_outputSink;
}

bool PlaybackController::showingRemote() const {
    return m_showingRemote;
}

void PlaybackController::bindVideoOutput(QVideoSink *sink) {
    if (sink == nullptr)
        return;

    if (m_outputSink == sink)
        return;                         // 已经绑过了

    m_outputSink = sink;

    // 本地解码帧到达时往外发一份 —— 推流侧订阅这个信号。
    connect(m_outputSink, &QVideoSink::videoFrameChanged,
            this, &PlaybackController::onVideoFrame);

    // 向sink送帧的对象可能是播放器也可能是远端渲染器,取决于显示本地还是远端。
    applyVideoSink();

    // 将sink交给 WebrtcManager,让远端渲染器往这里送帧。
    if (m_webrtcManager != nullptr)
        m_webrtcManager->setRemoteVideoSink(m_outputSink);

    qDebug() << "视频输出端已绑定:" << sink;
    emit displayVideoSinkChanged();
}

void PlaybackController::setRemoteRenderer(WebrtcManager *manager) {
    m_webrtcManager = manager;

    // 如果 sink 已经交过来了(顺序反过来),立刻接上
    if (m_webrtcManager != nullptr && m_outputSink != nullptr)
        m_webrtcManager->setRemoteVideoSink(m_outputSink);
}

void PlaybackController::setShowingRemote(bool showing) {
    if (m_showingRemote == showing)
        return;

    m_showingRemote = showing;


    // 显示本地 → 播放器往 sink 送(远端渲染器停手)
    // 显示远端 → 远端渲染器往 sink 送(播放器停手)
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

        qDebug() << "[诊断] applyVideoSink: 显示远端,播放器已脱钩";
    } else {
        // 显示本地画面,播放器往 sink 送帧
        m_player->setVideoSink(m_outputSink);

        // 停止远端渲染器
        if (m_webrtcManager != nullptr)
            m_webrtcManager->setRemoteVideoEnabled(false);

        qDebug() << "[诊断] applyVideoSink: 显示本地,播放器 →" << m_outputSink
                 << " 实际生效:" << m_player->videoSink();
    }
}



// ============================
// 加载视频
// ============================

void PlaybackController::load(const QUrl &source)
{
    if (!source.isValid())
        return;

    qDebug() << "Loading media:" << source;

    m_player->setSource(source);
    emit sourceChanged();
    emit hasLoadedChanged();
}

void PlaybackController::unload() {
    m_player->stop();
    m_player->setSource(QUrl());
    emit sourceChanged();
    emit hasLoadedChanged();
}


// ============================
// 播放
// ============================

void PlaybackController::play()
{
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
