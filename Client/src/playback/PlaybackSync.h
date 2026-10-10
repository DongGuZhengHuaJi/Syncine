//
// 播放同步策略 —— 唯一同时认识「房间」和「本地播放器」的地方。
//
// 为什么单独成类:
//   加载门禁(本地模式要全员加载完才能播)和回声抑制(自己执行远端命令时
//   不能反过来再广播)这两个判断,都需要同时看到房间状态和播放器状态。
//   —— 谁需要同时知道两边,谁就单独成一层。
//
// 职责四条:
//   1. 本地用户操作  -> 过门禁 -> 决定是否广播
//   2. 远端命令      -> 应用到播放器(期间抑制回声广播)
//   3. 媒体状态变化  -> 上报 video_status 给服务端
//   4. 统一的"播放状态外观" —— 界面只认这一个对象:
//        房主/本地模式:返回本地播放器的值(行为与直接问播放器一致)
//        观众(共享/网链):返回从房主同步来的值
//      控制入口(play/pause/seek)同理:
//        房主 -> 驱动本地播放器,经既有广播路径同步出去
//        观众 -> 直接给房间发命令,由房主执行后回传状态
//

#ifndef SYNCINE_PLAYBACKSYNC_H
#define SYNCINE_PLAYBACKSYNC_H

#include <QObject>
#include <QTimer>

#include <qqmlintegration.h>

#include "session/RoomSession.h"

class PlaybackController;

class PlaybackSync : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("由 main 创建并注入")

    Q_PROPERTY(bool canPlay
               READ canPlay
               NOTIFY gateChanged)

    Q_PROPERTY(BlockReason blockReason
               READ blockReason
               NOTIFY gateChanged)

    // ---- 统一的播放状态(界面只读这几个) ----
    // 房主/本地模式 = 本地播放器;观众 = 房主同步来的状态
    Q_PROPERTY(qint64 duration
               READ duration
               NOTIFY durationChanged)

    Q_PROPERTY(qint64 position
               READ position
               NOTIFY positionChanged)

    Q_PROPERTY(bool playing
               READ playing
               NOTIFY playingChanged)

    Q_PROPERTY(bool seekable
               READ seekable
               NOTIFY seekableChanged)

public:
    // 门禁不放行的原因。只给语义,具体文案留给 QML(界面的事归界面)
    enum class BlockReason {
        None,
        LocalNotLoaded,     // 自己还没加载视频
        OthersNotLoaded,    // 还有别人没加载
    };
    Q_ENUM(BlockReason)

    PlaybackSync(RoomSession *session,
                 PlaybackController *playback,
                 QObject *parent = nullptr);
    ~PlaybackSync() override = default;

    bool canPlay() const;
    BlockReason blockReason() const;

    // ---- 统一状态 ----
    qint64 duration() const;
    qint64 position() const;
    bool playing() const;
    bool seekable() const;

    // ---- 统一控制入口(界面只调这几个) ----
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlayPause();
    Q_INVOKABLE void seek(qint64 position);
    Q_INVOKABLE void seekRelative(qint64 offset);

signals:
    void gateChanged();
    void durationChanged();
    void positionChanged();
    void playingChanged();
    void seekableChanged();

private slots:
    void onLocalPlayingChanged();
    void onLocalSeeked(qint64 position);
    void onRemoteCommand(RoomSession::PlaybackAction action, qint64 position);
    void onPositionUpdate(qint64 position, bool playing);
    void onMediaChanged();
    void onRoomChanged();
    void onPositionTimer();
    void onPlaylistSwitched(const QString &itemId);

private:
    void pushVideoStatus();
    // 按当前模式解析某一条的播放源并加载;没有可用的源就把播放器卸载掉
    void applyPlaylistItem(const QString &itemId);
    void updateSyncedState(qint64 position, bool playing);
    // 从成员表里取房主的视频时长(观众端的 duration 来源)
    qint64 hostDuration() const;

    RoomSession *m_session = nullptr;
    PlaybackController *m_playback = nullptr;

    // 正在执行远端命令 —— 这期间播放器产生的变化都算"远端造成的",
    // 不是用户操作,不能再广播回去(否则 A→B→A 无限回声)
    bool m_applyingRemote = false;

    // 当前是否处于"观众看远端"的状态(共享/网链 + 非房主)。
    // 状态和控制入口都按它分流:观众用同步值,房主用本地值。
    bool m_showRemote = false;

    // ---- 观众端的同步状态(来自房主的广播) ----
    qint64 m_syncedPosition = 0;
    bool m_syncedPlaying = false;
    qint64 m_syncedDuration = 0;

    // 房主周期性广播自己的播放位置,观众端据此推进度条。
    // 只有"共享/网链 + 房主 + 播放中"时真正发出,其余时刻空转。
    QTimer m_positionTimer;
};

#endif //SYNCINE_PLAYBACKSYNC_H
