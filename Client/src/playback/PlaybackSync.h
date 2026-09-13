//
// 播放同步策略 —— 唯一同时认识「房间」和「本地播放器」的地方。
//
// 为什么要单独成类:
//   加载门禁(本地模式要全员加载完才能播)和回声抑制(自己执行远端命令时
//   不能反过来再广播)这两个判断,都需要同时看到房间状态和播放器状态。
//   塞进 RoomSession,房间就被迫认识 QMediaPlayer;
//   塞进 QML,同样的判断会在多个页面里重复三遍。
//   —— 谁需要同时知道两边,谁就单独成一层。
//
// 职责三条:
//   1. 本地用户操作  -> 过门禁 -> 决定是否广播
//   2. 远端命令      -> 应用到播放器(期间抑制回声广播)
//   3. 媒体状态变化  -> 上报 video_status 给服务端
//

#ifndef SYNCINE_PLAYBACKSYNC_H
#define SYNCINE_PLAYBACKSYNC_H

#include <QObject>

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

signals:
    void gateChanged();

private slots:
    void onLocalPlayingChanged();
    void onLocalSeeked(qint64 position);
    void onRemoteCommand(RoomSession::PlaybackAction action, qint64 position);
    void onMediaChanged();
    void onRoomChanged();

private:
    void pushVideoStatus();

    RoomSession *m_session = nullptr;
    PlaybackController *m_playback = nullptr;

    // 正在执行远端命令 —— 这期间播放器产生的变化都算"远端造成的",
    // 不是用户操作,不能再广播回去(否则 A→B→A 无限回声)
    bool m_applyingRemote = false;
};

#endif //SYNCINE_PLAYBACKSYNC_H
