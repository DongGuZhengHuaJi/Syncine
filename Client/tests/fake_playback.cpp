//
// PlaybackController 的最小实现，供 PlaybackSync 测试使用。
//
// 只实现测试会用到（以及 moc 的属性 / Q_INVOKABLE 表会引用）的成员。
// 不链接真实的 PlaybackController.cpp：那个版本会创建 QMediaPlayer 并解码，
// 测试不需要，也没法断言"到底调没调 load()"。
//
// 注意：如果 PlaybackController 新增了 Q_INVOKABLE 或属性，这里会出现链接
// 错误，按错误补一个空实现即可 —— 这比测试悄悄失效要好。
//

#include "fake_playback.h"

#include "playback/PlaybackController.h"

namespace {

FakePlaybackLog g_log;

} // namespace

FakePlaybackLog &fakePlayback()
{
    return g_log;
}

void FakePlaybackLog::clear()
{
    loads.clear();
    unloads = 0;
    seeks.clear();
}

// ============================
// 测试替身
// ============================

PlaybackController::PlaybackController(QObject *parent)
    : QObject(parent)
{
}

bool PlaybackController::hasLoaded() const
{
    return g_log.source.isValid();
}

bool PlaybackController::playing() const
{
    return false;
}

bool PlaybackController::seekable() const
{
    return g_log.seekable;
}

qint64 PlaybackController::position() const
{
    return 0;
}

qint64 PlaybackController::duration() const
{
    return 0;
}

qreal PlaybackController::playbackRate() const
{
    return 1.0;
}

void PlaybackController::setPlaybackRate(qreal)
{
}

double PlaybackController::volume() const
{
    return 1.0;
}

void PlaybackController::setVolume(double)
{
}

bool PlaybackController::muted() const
{
    return false;
}

void PlaybackController::setMuted(bool)
{
}

void PlaybackController::setOutputDevice(const QAudioDevice &)
{
}

QString PlaybackController::hash() const
{
    return {};
}

QMediaPlayer *PlaybackController::player() const
{
    return nullptr;
}

QVideoSink *PlaybackController::displayVideoSink() const
{
    return nullptr;
}

QUrl PlaybackController::source() const
{
    return g_log.source;
}

bool PlaybackController::showingRemote() const
{
    return g_log.showingRemote;
}

void PlaybackController::setShowingRemote(bool showing)
{
    if (g_log.showingRemote == showing)
        return;
    g_log.showingRemote = showing;
    emit showingRemoteChanged();
}

void PlaybackController::bindVideoOutput(QVideoSink *)
{
}

void PlaybackController::unbindVideoOutput()
{
}

void PlaybackController::setRemoteRenderer(WebrtcManager *)
{
}

void PlaybackController::load(const QUrl &source)
{
    if (!source.isValid())
        return;

    g_log.loads.append(source);
    g_log.source = source;
    // 真实播放器会发这两个信号；PlaybackSync 的 onMediaChanged 会因此上报
    // video_status，而那条上报又会绕回成员表刷新。测试要覆盖这个回声路径。
    emit sourceChanged();
    emit hasLoadedChanged();
}

void PlaybackController::unload()
{
    ++g_log.unloads;
    g_log.source.clear();
    emit sourceChanged();
    emit hasLoadedChanged();
}

void PlaybackController::play()
{
}

void PlaybackController::pause()
{
}

void PlaybackController::togglePlayPause()
{
}

void PlaybackController::seek(qint64 position)
{
    g_log.seeks.append(position);
}

void PlaybackController::seekRelative(qint64)
{
}

void PlaybackController::toggleMute()
{
}
