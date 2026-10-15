//
// 播放同步策略：唯一同时认识「房间」与「本地播放器」的地方。
//
// 独立成类的原因：加载门禁（同步模式需全员加载完才能播）与回声抑制
// （执行远端命令时不能反向再广播）这两个判断都需要同时看到房间状态与播放器状态。
//
// 职责四条：
//   1. 本地用户操作 → 过门禁 → 决定是否广播
//   2. 远端命令 → 应用到播放器（期间抑制回声广播）
//   3. 媒体状态变化 → 上报 video_status 给服务端
//   4. 统一的播放状态外观（界面只认这一个对象）：
//        房主 / 同步模式：返回本地播放器的值（与直接询问播放器一致）
//        观众（共享/网链）：返回从房主同步来的值
//      控制入口（play/pause/seek）同理：房主驱动本地播放器并经既有广播路径同步；
//      观众直接向房间发命令，由房主执行后回传状态。
//

#ifndef SYNCINE_PLAYBACKSYNC_H
#define SYNCINE_PLAYBACKSYNC_H

#include <QObject>
#include <QTimer>

#include <qqmlintegration.h>

#include "session/RoomSession.h"

class PlaybackController;
class UrlDownloader;

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

    // ---- 统一的播放状态（界面只读这几个） ----
    // 房主/本地模式 = 本地播放器；观众 = 房主同步来的状态
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

    // ---- 网链模式的准备进度（给界面显示） ----
    // 解析中 / 下载中 / 合并中 / 就绪 / 失败。共享模式和同步模式下恒为空。
    Q_PROPERTY(QString downloadStage
               READ downloadStage
               NOTIFY downloadStateChanged)

    Q_PROPERTY(int downloadProgress
               READ downloadProgress
               NOTIFY downloadStateChanged)

    Q_PROPERTY(QString downloadDetail
               READ downloadDetail
               NOTIFY downloadStateChanged)

    Q_PROPERTY(QString downloadError
               READ downloadError
               NOTIFY downloadStateChanged)

    // 状态的中文文案（"解析中…"/"下载中 42% · 8.2MB/s"/"合并中…"/"下载失败"）。
    // 放在 C++ 里是为了让播放器提示和列表条目用的是同一份文案，不会两处各写一遍。
    Q_PROPERTY(QString downloadStageText
               READ downloadStageText
               NOTIFY downloadStateChanged)

    // 当前条目是不是直播源。界面用它把进度条/时间换成"直播"标识。
    Q_PROPERTY(bool live
               READ live
               NOTIFY liveChanged)

public:
    // 门禁不放行的原因。只给语义，具体文案留给 QML(界面的事归界面)
    enum class BlockReason {
        None,
        LocalNotLoaded,     // 自己还没加载视频
        OthersNotLoaded,    // 还有别人没加载
    };
    Q_ENUM(BlockReason)

    PlaybackSync(RoomSession *session,
                 PlaybackController *playback,
                 UrlDownloader *urlDownloader,
                 QObject *parent = nullptr);
    ~PlaybackSync() override = default;

    bool canPlay() const;
    BlockReason blockReason() const;

    QString downloadStage() const;
    int downloadProgress() const;
    QString downloadDetail() const;
    QString downloadError() const;
    QString downloadStageText() const;
    bool live() const;

    // 网链模式：重新准备当前条目（解析或下载失败后点"重试"）
    Q_INVOKABLE void retryCurrentItem();

    // ---- 统一状态 ----
    qint64 duration() const;
    qint64 position() const;
    bool playing() const;
    bool seekable() const;

    // ---- 统一控制入口（界面只调这几个） ----
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
    void downloadStateChanged();
    void liveChanged();

private slots:
    void onLocalPlayingChanged();
    void onLocalSeeked(qint64 position);
    void onRemoteCommand(RoomSession::PlaybackAction action, qint64 position);
    void onPositionUpdate(qint64 position, bool playing);
    void onMediaChanged();
    void onRoomChanged();
    void onPositionTimer();
    void onPlaylistSwitched(const QString &itemId);
    // 网链模式：解析出直链（路径 A,秒开）或开始下载合并（路径 B）
    void onUrlResolved();
    void onUrlReady(const QString &filePath);

private:
    void pushVideoStatus();
    // 按当前模式解析某一条的播放源并加载；没有可用的源就把播放器卸载掉
    void applyPlaylistItem(const QString &itemId);
    // 网链模式：准备某一条（解析 → 直链播放 或 下载合并）
    void prepareUrlItem(const QString &itemId);
    // 停掉可能在跑的下载。**不删文件** —— 下载好的那份留着，切回这一条直接复用
    // (缓存只在退出软件时清，见 removeRoomTempDir 的说明)
    void cancelOngoingDownload();
    void updateSyncedState(qint64 position, bool playing);
    // 从成员表里取房主的视频时长（观众端的 duration 来源）
    qint64 hostDuration() const;

    RoomSession *m_session = nullptr;
    PlaybackController *m_playback = nullptr;
    UrlDownloader *m_urlDownloader = nullptr;

    // 网链模式：正在准备（解析/下载）的条目，和已经准备好、临时文件还在的条目
    QString m_preparingItemId;
    QString m_preparedItemId;

    // 正在执行远端命令 —— 这期间播放器产生的变化都算"远端造成的",
    // 不是用户操作，不能再广播回去（否则 A→B→A 无限回声）
    bool m_applyingRemote = false;

    // 当前是否处于"观众看远端"的状态（共享/网链 + 非房主）。
    // 状态和控制入口都按它分流：观众用同步值，房主用本地值。
    bool m_showRemote = false;

    // ---- 观众端的同步状态（来自房主的广播） ----
    qint64 m_syncedPosition = 0;
    bool m_syncedPlaying = false;
    qint64 m_syncedDuration = 0;

    // 房主周期性广播自己的播放位置，观众端据此推进度条。
    // 只有"共享/网链 + 房主 + 播放中"时真正发出，其余时刻空转。
    QTimer m_positionTimer;
};

#endif //SYNCINE_PLAYBACKSYNC_H
