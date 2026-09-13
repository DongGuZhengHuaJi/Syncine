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

    // ---- 房间 -> 本地播放器 ----
    connect(m_session, &RoomSession::playbackCommandReceived,
            this, &PlaybackSync::onRemoteCommand);

    // ---- 房间状态变化 -> 重算门禁 ----
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
                emit gateChanged();
            });
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
}

void PlaybackSync::onRoomChanged() {
    emit gateChanged();
}

void PlaybackSync::pushVideoStatus() {
    if (m_session == nullptr || m_playback == nullptr || !m_session->inRoom())
        return;

    m_session->reportVideoStatus(m_playback->hasLoaded(),
                                 m_playback->hash(),
                                 m_playback->duration());
}

// ============================
// 房间 -> 本地
// ============================

void PlaybackSync::onRemoteCommand(RoomSession::PlaybackAction action, qint64 position) {
    if (m_session == nullptr || m_playback == nullptr)
        return;

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
