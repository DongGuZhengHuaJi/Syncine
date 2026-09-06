//
// Created by donggu on 2026/9/6.
//

#include "PlaybackController.h"

#include <QMediaPlayer>
#include <QAudioOutput>
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

    m_audioOutput->setVolume(volume);

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


// ============================
// 获取 QMediaPlayer
// ============================

QMediaPlayer *PlaybackController::player() const
{
    return m_player;
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
