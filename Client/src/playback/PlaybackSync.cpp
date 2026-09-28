//
// Created by donggu on 2026/9/13.
//

#include "PlaybackSync.h"

#include "playback/PlaybackController.h"

namespace {

// RAII:在这个作用域内,播放器的一切变化都算"远端命令的结果",不广播。
// 用 RAII 而不是手动置位/清零,是为了中途 return 或抛异常也不会漏掉复位。
class RemoteScope {
public:
    explicit RemoteScope(bool &flag)
        : m_flag(flag) {
        m_flag = true;
    }

    ~RemoteScope() {
        m_flag = false;
    }

    RemoteScope(const RemoteScope &) = delete;
    RemoteScope &operator=(const RemoteScope &) = delete;

private:
    bool &m_flag;
};

} // namespace

PlaybackSync::PlaybackSync(RoomSession *session,
                           PlaybackController *playback,
                           QObject *parent)
    : QObject(parent),
      m_session(session),
      m_playback(playback) {

    if (m_session == nullptr || m_playback == nullptr)
        return;

    // ---- 本地播放器 -> 房间 ----
    connect(m_playback, &PlaybackController::playingChanged,
            this, &PlaybackSync::onLocalPlayingChanged);
    connect(m_playback, &PlaybackController::userSeeked,
            this, &PlaybackSync::onLocalSeeked);
    connect(m_playback, &PlaybackController::sourceChanged,
            this, &PlaybackSync::onMediaChanged);
    connect(m_playback, &PlaybackController::durationChanged,
            this, &PlaybackSync::onMediaChanged);

    // 房主/本地模式:界面状态跟着本地播放器走,这里把变化转发出去
    connect(m_playback, &PlaybackController::playingChanged,
            this, &PlaybackSync::playingChanged);
    connect(m_playback, &PlaybackController::positionChanged,
            this, &PlaybackSync::positionChanged);
    connect(m_playback, &PlaybackController::durationChanged,
            this, &PlaybackSync::durationChanged);
    connect(m_playback, &PlaybackController::seekableChanged,
            this, &PlaybackSync::seekableChanged);

    // ---- 房间 -> 本地播放器 / 同步状态 ----
    connect(m_session, &RoomSession::playbackCommandReceived,
            this, &PlaybackSync::onRemoteCommand);
    connect(m_session, &RoomSession::playbackPositionReceived,
            this, &PlaybackSync::onPositionUpdate);

    // ---- 房间状态变化 -> 重算门禁 / 同步状态 ----
    connect(m_session, &RoomSession::membersChanged,
            this, &PlaybackSync::onRoomChanged);
    connect(m_session, &RoomSession::roomModeChanged,
            this, &PlaybackSync::onRoomChanged);
    connect(m_session, &RoomSession::left,
            this, &PlaybackSync::onRoomChanged);
    connect(m_session, &RoomSession::entered,
            this, [this]() {
                // 进房之前可能就已经加载好视频了,补报一次,
                // 否则服务端不知道"这个成员能播"
                pushVideoStatus();
                onRoomChanged();
            });

    // 房主周期性广播位置(观众端进度条的数据源)
    m_positionTimer.setInterval(500);
    connect(&m_positionTimer, &QTimer::timeout,
            this, &PlaybackSync::onPositionTimer);
    m_positionTimer.start();
}

// ============================
// 统一状态
// ============================

qint64 PlaybackSync::duration() const {
    if (m_showRemote)
        return m_syncedDuration;

    return m_playback != nullptr ? m_playback->duration() : 0;
}

qint64 PlaybackSync::position() const {
    if (m_showRemote)
        return m_syncedPosition;

    return m_playback != nullptr ? m_playback->position() : 0;
}

bool PlaybackSync::playing() const {
    if (m_showRemote)
        return m_syncedPlaying;

    return m_playback != nullptr && m_playback->playing();
}

bool PlaybackSync::seekable() const {
    if (m_showRemote)
        return m_syncedDuration > 0;      // 知道时长就能拖进度条

    return m_playback != nullptr && m_playback->seekable();
}

// ============================
// 统一控制入口
// ============================

void PlaybackSync::play() {
    // 观众:自己没有媒体,直接给房间发命令。
    // 乐观置位让界面立刻响应;房主执行后会周期回传状态做最终确认。
    if (m_showRemote && m_session->inRoom()) {
        m_session->sendPlayback(RoomSession::PlaybackAction::Play, m_syncedPosition);
        updateSyncedState(m_syncedPosition, true);
        return;
    }

    // 房主/本地:驱动本地播放器,广播由 playingChanged → onLocalPlayingChanged 完成
    if (m_playback != nullptr)
        m_playback->play();
}

void PlaybackSync::pause() {
    if (m_showRemote && m_session->inRoom()) {
        m_session->sendPlayback(RoomSession::PlaybackAction::Pause, m_syncedPosition);
        updateSyncedState(m_syncedPosition, false);
        return;
    }

    if (m_playback != nullptr)
        m_playback->pause();
}

void PlaybackSync::togglePlayPause() {
    if (playing())
        pause();
    else
        play();
}

void PlaybackSync::seek(qint64 position) {
    if (m_showRemote && m_session->inRoom()) {
        const qint64 clamped = qBound<qint64>(0, position, m_syncedDuration);
        m_session->sendPlayback(RoomSession::PlaybackAction::Seek, clamped);
        updateSyncedState(clamped, m_syncedPlaying);
        return;
    }

    // 房主/本地:本地跳转,广播由 userSeeked → onLocalSeeked 完成
    if (m_playback != nullptr)
        m_playback->seek(position);
}

void PlaybackSync::seekRelative(qint64 offset) {
    seek(position() + offset);
}

// ============================
// 门禁
// ============================

PlaybackSync::BlockReason PlaybackSync::blockReason() const {
    if (m_session == nullptr || m_playback == nullptr)
        return BlockReason::None;

    // 只有本地模式需要"全员加载完才能播":
    // 共享/网链模式下视频由房主提供,不存在成员各加载各的
    if (m_session->roomMode() != RoomSession::RoomMode::Local)
        return BlockReason::None;

    if (!m_playback->hasLoaded())
        return BlockReason::LocalNotLoaded;

    if (m_session->inRoom() && !m_session->allOthersLoaded())
        return BlockReason::OthersNotLoaded;

    return BlockReason::None;
}

bool PlaybackSync::canPlay() const {
    return blockReason() == BlockReason::None;
}

// ============================
// 本地 -> 房间
// ============================

void PlaybackSync::onLocalPlayingChanged() {
    if (m_applyingRemote || !m_session->inRoom())
        return;

    if (!m_playback->playing()) {
        m_session->sendPlayback(RoomSession::PlaybackAction::Pause,
                                m_playback->position());
        return;
    }

    if (!canPlay()) {
        // 门禁拦截:退回暂停。注意这里**不广播** ——
        // 广播出去会把别人也按停,而他本来就不该被这条操作影响
        const RemoteScope scope(m_applyingRemote);
        m_playback->pause();
        return;
    }

    m_session->sendPlayback(RoomSession::PlaybackAction::Play,
                            m_playback->position());
}

void PlaybackSync::onLocalSeeked(qint64 position) {
    if (m_applyingRemote || !m_session->inRoom())
        return;

    m_session->sendPlayback(RoomSession::PlaybackAction::Seek, position);
}

void PlaybackSync::onMediaChanged() {
    pushVideoStatus();
    emit gateChanged();
    emit durationChanged();
}

void PlaybackSync::onRoomChanged() {
    if (m_session == nullptr || m_playback == nullptr)
        return;

    // 该显示哪一路画面:
    //
    //   本地模式           → 自己的画面(每人各自加载各自的文件)
    //   共享/网链 + 房主   → 自己的画面(房主就是推流的那一方)
    //   共享/网链 + 非房主 → 远端画面(房主推过来的)
    const bool isLocal = (m_session->roomMode() == RoomSession::RoomMode::Local);
    m_showRemote = !isLocal && !m_session->isHost();

    m_playback->setShowingRemote(m_showRemote);

    // 观众端的时长来自房主的 video_status(成员表里就有)
    m_syncedDuration = hostDuration();

    emit durationChanged();
    emit seekableChanged();
    emit positionChanged();
    emit playingChanged();
    emit gateChanged();
}

void PlaybackSync::onPositionTimer() {
    if (m_session == nullptr || m_playback == nullptr)
        return;

    // 只有"推流方"需要周期广播位置;观众端据此画进度条。
    // 其余时刻这个定时器空转,开销可忽略。
    if (!m_session->inRoom() || !m_session->isHost())
        return;
    if (m_session->roomMode() == RoomSession::RoomMode::Local)
        return;
    if (!m_playback->playing())
        return;

    m_session->sendPlaybackPosition(m_playback->position(), true);
}

void PlaybackSync::pushVideoStatus() {
    if (m_session == nullptr || m_playback == nullptr || !m_session->inRoom())
        return;

    m_session->reportVideoStatus(m_playback->hasLoaded(),
                                 m_playback->hash(),
                                 m_playback->duration());
}

// ============================
// 房间 -> 本地 / 同步状态
// ============================

void PlaybackSync::onRemoteCommand(RoomSession::PlaybackAction action, qint64 position) {
    if (m_session == nullptr || m_playback == nullptr)
        return;

    // 观众端:命令是"状态更新",不是驱动本地播放器(观众没有媒体)
    if (m_showRemote) {
        updateSyncedState(position, action == RoomSession::PlaybackAction::Play);
        return;
    }

    const RemoteScope scope(m_applyingRemote);

    // 三种动作都先对齐位置,再执行动作
    m_playback->seek(position);

    switch (action) {
    case RoomSession::PlaybackAction::Play:
        // 本地模式下还有人没加载完就先不播,停在原地等
        if (canPlay())
            m_playback->play();
        break;

    case RoomSession::PlaybackAction::Pause:
        m_playback->pause();
        break;

    case RoomSession::PlaybackAction::Seek:
        break;
    }
}

void PlaybackSync::onPositionUpdate(qint64 position, bool playing) {
    if (!m_showRemote)
        return;                     // 房主/本地不需要这个

    updateSyncedState(position, playing);
}

void PlaybackSync::updateSyncedState(qint64 position, bool playing) {
    bool changed = false;

    if (m_syncedPosition != position) {
        m_syncedPosition = position;
        changed = true;
        emit positionChanged();
    }

    if (m_syncedPlaying != playing) {
        m_syncedPlaying = playing;
        changed = true;
        emit playingChanged();
    }

    if (changed)
        emit gateChanged();
}

qint64 PlaybackSync::hostDuration() const {
    if (m_session == nullptr || m_session->members() == nullptr)
        return 0;

    // 成员数只有个位数,线性扫描足够
    for (const Member &member : m_session->members()->members()) {
        if (member.isHost)
            return member.duration;
    }
    return 0;
}
