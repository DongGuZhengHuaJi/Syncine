//
// Created by donggu on 2026/9/13.
//

#include "PlaybackSync.h"

#include <QFile>
#include <QFileInfo>

#include "core/Log.h"
#include "core/MediaHash.h"
#include "core/UrlDownloader.h"
#include "danmaku/DanmakuController.h"
#include "media/FormatModel.h"
#include "playback/PlaybackController.h"

namespace {

// RAII:在这个作用域内，播放器的一切变化都算"远端命令的结果",不广播。
// 用 RAII 而不是手动置位/清零，是为了中途 return 或抛异常也不会漏掉复位。
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
                           UrlDownloader *urlDownloader,
                           DanmakuController *danmaku,
                           QObject *parent)
    : QObject(parent),
      m_session(session),
      m_playback(playback),
      m_urlDownloader(urlDownloader),
      m_danmaku(danmaku) {

    if (m_session == nullptr || m_playback == nullptr)
        return;

    // ---- 网链模式：解析/下载的结果 ----
    if (m_urlDownloader != nullptr) {
        connect(m_urlDownloader, &UrlDownloader::resolved,
                this, &PlaybackSync::onUrlResolved);
        connect(m_urlDownloader, &UrlDownloader::probed,
                this, &PlaybackSync::onUrlProbed);
        connect(m_urlDownloader, &UrlDownloader::ready,
                this, &PlaybackSync::onUrlReady);
        connect(m_urlDownloader, &UrlDownloader::stateChanged,
                this, &PlaybackSync::downloadStateChanged);
        connect(m_urlDownloader, &UrlDownloader::failed,
                this, &PlaybackSync::onUrlFailed);
    }

    // ---- 本地播放器 -> 房间 ----
    connect(m_playback, &PlaybackController::playingChanged,
            this, &PlaybackSync::onLocalPlayingChanged);
    connect(m_playback, &PlaybackController::userSeeked,
            this, &PlaybackSync::onLocalSeeked);
    connect(m_playback, &PlaybackController::sourceChanged,
            this, &PlaybackSync::onMediaChanged);
    connect(m_playback, &PlaybackController::durationChanged,
            this, &PlaybackSync::onMediaChanged);

    // 房主/本地模式：界面状态跟着本地播放器走，这里把变化转发出去
    connect(m_playback, &PlaybackController::playingChanged,
            this, &PlaybackSync::playingChanged);
    connect(m_playback, &PlaybackController::positionChanged,
            this, &PlaybackSync::positionChanged);
    connect(m_playback, &PlaybackController::durationChanged,
            this, &PlaybackSync::durationChanged);
    connect(m_playback, &PlaybackController::seekableChanged,
            this, &PlaybackSync::seekableChanged);
    // 本地换挡后界面要立刻更新；远端命令应用时也会走到这里（统一外观）
    connect(m_playback, &PlaybackController::playbackRateChanged,
            this, [this](qreal) { emit playbackRateChanged(); });

    // ---- 房间 -> 本地播放器 / 同步状态 ----
    connect(m_session, &RoomSession::playbackCommandReceived,
            this, &PlaybackSync::onRemoteCommand);

    // seekable() 现在也取决于共享态。QML 对 seekable 的绑定只会在
    // seekableChanged 时重算，共享开始/结束时必须显式通知一次，
    // 否则停止共享后进度条会停留在禁用状态。
    connect(m_session, &RoomSession::shareStateChanged,
            this, [this] {
                emit seekableChanged();
                emit gateChanged();
            });
    connect(m_session, &RoomSession::playbackPositionReceived,
            this, &PlaybackSync::onPositionUpdate);

    // ---- 房间状态变化 -> 重算门禁 / 同步状态 ----
    // membersChanged 只刷新派生状态；只有房主身份变化才可能改变媒体源。
    // 房间模式/进出房才走完整的重算路径，避免成员表每次刷新都重建解码器。
    connect(m_session, &RoomSession::membersChanged,
            this, &PlaybackSync::onMembersChanged);
    connect(m_session, &RoomSession::roomModeChanged,
            this, &PlaybackSync::onRoomChanged);
    connect(m_session, &RoomSession::left,
            this, &PlaybackSync::onRoomChanged);

    // 房间的"当前条目"变了 —— 播放源由这里决定，是唯一会调用 load/unload 的地方
    connect(m_session, &RoomSession::playlistSwitched,
            this, &PlaybackSync::onPlaylistSwitched);
    // 条目被服务端确认删除：即使不是当前条目，也要停掉它的解析/下载。
    connect(m_session->playlist(), &PlaylistModel::itemRemoved,
            this, &PlaybackSync::onPlaylistItemRemoved);
    connect(m_session, &RoomSession::entered,
            this, [this]() {
                // 进房之前可能就已经加载好视频了，补报一次，
                // 否则服务端不知道"这个成员能播"
                pushVideoStatus();
                onRoomChanged();
            });

    // ---- 直播掉线重连 ----
    // 这里自动重新解析一遍再加载，延迟 3 秒避免疯狂重试。
    connect(m_playback, &PlaybackController::errorOccurred, this,
            [this](const QString &message) {
                if (m_session == nullptr || !m_session->inRoom())
                    return;
                if (m_session->roomMode() != RoomSession::RoomMode::Url || !live())
                    return;

                LOG_WARN("Playback") << "直播流出错,3 秒后重连:" << message;
                QTimer::singleShot(3000, this, [this]() {
                    if (m_session == nullptr || !m_session->inRoom())
                        return;
                    if (m_session->roomMode() != RoomSession::RoomMode::Url || !live())
                        return;

                    // 清掉"已就绪"标记，让 prepareUrlItem 重新走一遍解析
                    clearPrepared();
                    clearDownloadItem();
                    prepareUrlItem(m_session->playlist()->currentItemId());
                });
            });

    // 房主周期性广播位置（观众端进度条的数据源）
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

bool PlaybackSync::screenSharing() const {
    return m_session != nullptr && m_session->screenSharing();
}

bool PlaybackSync::seekable() const {
    if (screenSharing())
        return false;

    if (m_showRemote)
        return m_syncedDuration > 0;      // 知道时长就能拖进度条

    return m_playback != nullptr && m_playback->seekable();
}

qreal PlaybackSync::playbackRate() const {
    if (m_showRemote)
        return m_syncedRate;

    return m_playback != nullptr ? m_playback->playbackRate() : 1.0;
}

void PlaybackSync::setPlaybackRate(qreal rate) {
    rate = qBound(0.25, rate, 4.0);

    // 共享模式第一版不支持倍速：电影音频是"原始 PCM 按实时节奏推给
    // WebRTC"的，换挡后采样到达速度会跟着变，接收端只能靠 NetEq 变速/丢包，
    // 表现为断续。直播也没有共同时间轴，播放器对直播换挡的支持不可靠。
    if (m_session != nullptr && m_session->inRoom()
        && m_session->roomMode() == RoomSession::RoomMode::Share)
        return;
    if (live())
        return;

    if (qFuzzyCompare(playbackRate(), rate))
        return;

    // 观众：本地没有媒体，只更新同步状态并把换挡命令发出去
    if (m_showRemote) {
        m_syncedRate = rate;
        emit playbackRateChanged();
        if (m_session->inRoom())
            m_session->sendPlayback(RoomSession::PlaybackAction::Rate,
                                    m_syncedPosition, rate);
        return;
    }

    if (m_playback == nullptr)
        return;

    m_playback->setPlaybackRate(rate);

    // 广播实际生效的值。播放器还没加载媒体时位置未知（-1），只同步倍速，
    // 不能让接收方按 0 去对齐 —— 那会把正在看的人拖回片头。
    const qint64 ratePosition = m_playback->hasLoaded()
                                    ? m_playback->position()
                                    : -1;
    if (m_session != nullptr && m_session->inRoom())
        m_session->sendPlayback(RoomSession::PlaybackAction::Rate,
                                ratePosition,
                                m_playback->playbackRate());
}

// 网链模式的准备进度。其它模式下下载器是空闲的，这几个值自然是空的。
// **只暴露当前条目的任务**：后台重试另一个条目时，不能让它的进度/错误
// 串到正在播放的这一行上（后台状态由 retryingItemId 单独表示）。
bool PlaybackSync::downloadIsCurrent() const {
    return m_session != nullptr && m_urlDownloader != nullptr
           && !m_downloadItemId.isEmpty()
           && m_downloadItemId == m_session->playlist()->currentItemId();
}

bool PlaybackSync::downloadFailedIsCurrent() const {
    return m_session != nullptr
           && m_failedItems.contains(m_session->playlist()->currentItemId());
}

QString PlaybackSync::downloadStage() const {
    if (downloadFailedIsCurrent())
        return QStringLiteral("failed");
    return downloadIsCurrent() ? m_urlDownloader->stage() : QString();
}

int PlaybackSync::downloadProgress() const {
    if (downloadFailedIsCurrent())
        return -1;
    return downloadIsCurrent() ? m_urlDownloader->progress() : -1;
}

QString PlaybackSync::downloadDetail() const {
    if (downloadFailedIsCurrent())
        return {};
    return downloadIsCurrent() ? m_urlDownloader->detailText() : QString();
}

QString PlaybackSync::downloadError() const {
    if (downloadFailedIsCurrent())
        return m_failedItems.value(m_session->playlist()->currentItemId());
    return downloadIsCurrent() ? m_urlDownloader->errorText() : QString();
}

bool PlaybackSync::live() const {
    if (m_session == nullptr)
        return false;
    return m_session->playlist()->currentIsLive();
}

QString PlaybackSync::downloadStageText() const
{
    const QString stage = downloadStage();
    if (stage.isEmpty())
        return {};
    if (stage == QLatin1String("probing"))
        return QStringLiteral("解析中…");
    if (stage == QLatin1String("downloading")) {
        const int progress = m_urlDownloader->progress();
        const QString detail = m_urlDownloader->detailText();
        if (progress < 0)
            return QStringLiteral("下载中…"); // 总大小未知
        return QStringLiteral("下载中 %1%").arg(progress)
               + (detail.isEmpty() ? QString() : QStringLiteral(" · ") + detail);
    }
    if (stage == QLatin1String("merging"))
        return QStringLiteral("合并音视频中…");
    if (stage == QLatin1String("checking"))
        return QStringLiteral("检查动态范围中…");
    if (stage == QLatin1String("failed"))
        return QStringLiteral("下载失败");
    return {};
}

QString PlaybackSync::downloadShortText() const
{
    const QString stage = downloadStage();
    if (stage.isEmpty())
        return {};
    if (stage == QLatin1String("probing"))
        return QStringLiteral("解析中");
    if (stage == QLatin1String("downloading")) {
        const int progress = m_urlDownloader->progress();
        return progress < 0 ? QStringLiteral("下载中")
                            : QStringLiteral("下载中 %1%").arg(progress);
    }
    if (stage == QLatin1String("merging"))
        return QStringLiteral("合并中");
    if (stage == QLatin1String("checking"))
        return QStringLiteral("检查中");
    if (stage == QLatin1String("failed"))
        return QStringLiteral("下载失败");
    return {};
}

QString PlaybackSync::retryingItemId() const
{
    if (m_downloadItemId.isEmpty() || m_session == nullptr)
        return {};
    return m_downloadItemId == m_session->playlist()->currentItemId()
               ? QString()
               : m_downloadItemId;
}

void PlaybackSync::clearDownloadItem()
{
    if (m_downloadItemId.isEmpty())
        return;
    m_downloadItemId.clear();
    emit retryStateChanged();
    // downloadStage/Progress/Error 都按“是否当前条目”门控，标记变化时
    // 也要让这些绑定重算
    emit downloadStateChanged();
}

void PlaybackSync::clearFailedItem(const QString &itemId)
{
    if (m_failedItems.remove(itemId) > 0)
        emit downloadStateChanged();
}

void PlaybackSync::clearAllFailedItems()
{
    if (m_failedItems.isEmpty())
        return;
    m_failedItems.clear();
    emit downloadStateChanged();
}

// ============================
// 网链模式：已就绪标记
// ============================

bool PlaybackSync::preparedSourceLoaded(const QString &itemId) const
{
    if (itemId.isEmpty() || itemId != m_preparedItemId || !m_preparedSource.isValid())
        return false;
    if (m_playback == nullptr || !m_playback->hasLoaded())
        return false;

    // 只看条目 id 不够：本地/网链模式往返后，播放器里的可能是同一个条目的
    // 另一种来源（本地文件 vs 缓存文件）。QUrl 的相等比较对本地文件的两种
    // 拼法是稳的，直接用它判断"播放器上现在就是准备好的那一份"。
    return m_playback->source() == m_preparedSource;
}

void PlaybackSync::markPrepared(const QString &itemId, const QUrl &source)
{
    m_preparedItemId = itemId;
    m_preparedSource = source;
}

void PlaybackSync::clearPrepared()
{
    m_preparedItemId.clear();
    m_preparedSource.clear();
}

void PlaybackSync::retryItem(const QString &itemId)
{
    if (m_session == nullptr || m_urlDownloader == nullptr)
        return;
    if (!m_session->inRoom() || m_session->roomMode() != RoomSession::RoomMode::Url)
        return;

    const QString current = m_session->playlist()->currentItemId();
    if (itemId.isEmpty() || !m_session->playlist()->contains(itemId))
        return;

    if (itemId == current) {
        LOG_INFO("Playback") << "重试当前条目";
        // 先掐掉可能在跑的那次（失败/卡住时它可能还挂着）
        m_urlDownloader->cancel();
        // 弹幕也一起复位；beginItem 仍会优先复用已有的 danmaku.xml，
        // 上次没下成功时才会重新拉取
        if (m_danmaku != nullptr)
            m_danmaku->reset();
        clearPrepared();
        clearFailedItem(itemId);
        clearDownloadItem();
        // "重试"的意思是**重新下一遍**:把缓存的映射删掉，否则 prepareUrlItem
        // 会认出上一轮的产物直接复用它（重试就变成空转）
        m_session->playlist()->setLocalFile(itemId, QString());
        prepareUrlItem(itemId);
        return;
    }

    // 后台重试：只在下载器空闲时做，避免打断当前条目的准备。
    // 当前条目如果已经在播，下载器通常是 idle/ready，这里可以放心下另一条。
    const QString stage = m_urlDownloader->stage();
    const bool busy = stage == QLatin1String("probing")
                      || stage == QLatin1String("downloading")
                      || stage == QLatin1String("merging")
                      || stage == QLatin1String("checking");
    if (busy) {
        LOG_INFO("Playback") << "下载器正忙,暂不后台重试:" << itemId
                             << " 当前阶段=" << stage;
        emit backgroundRetryFinished(
            itemId, false,
            QStringLiteral("当前有下载任务正在进行，请稍后再试"));
        return;
    }

    const QString url = m_session->urlFor(itemId);
    if (url.isEmpty()) {
        LOG_WARN("Playback") << "后台重试失败:条目没有 URL" << itemId;
        emit backgroundRetryFinished(itemId, false,
                                     QStringLiteral("该条目没有可解析的链接"));
        return;
    }

    LOG_INFO("Playback") << "后台重试条目:" << itemId << url;
    // 如果重试的正是之前失败的那一条，清掉失败标记；当前条目的失败
    // 标记（如果不同）保留，界面仍能看到它的错误。
    if (m_failedItems.contains(itemId))
        clearFailedItem(itemId);
    // 缓存的映射先清掉，否则 prepare 会直接复用旧产物
    m_session->playlist()->setLocalFile(itemId, QString());
    m_downloadItemId = itemId;
    emit retryStateChanged();

    m_urlDownloader->setDownloadDir(
        UrlDownloader::itemTempDir(m_session->roomId(), m_session->clientId(), itemId));
    m_urlDownloader->start(url, m_session->playlist()->specOf(itemId));
}

// ============================
// 统一控制入口
// ============================

void PlaybackSync::play() {
    if (screenSharing())
        return;

    // 观众：自己没有媒体，直接给房间发命令。
    // 乐观置位让界面立刻响应；房主执行后会周期回传状态做最终确认。
    if (m_showRemote && m_session->inRoom()) {
        m_session->sendPlayback(RoomSession::PlaybackAction::Play, m_syncedPosition);
        updateSyncedState(m_syncedPosition, true);
        return;
    }

    // 房主/本地：驱动本地播放器，广播由 playingChanged → onLocalPlayingChanged 完成
    if (m_playback != nullptr)
        m_playback->play();
}

void PlaybackSync::pause() {
    if (screenSharing())
        return;

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
    if (screenSharing())
        return;

    if (m_showRemote && m_session->inRoom()) {
        const qint64 clamped = qBound<qint64>(0, position, m_syncedDuration);
        m_session->sendPlayback(RoomSession::PlaybackAction::Seek, clamped);
        updateSyncedState(clamped, m_syncedPlaying);
        return;
    }

    // 房主/本地：本地跳转，广播由 userSeeked → onLocalSeeked 完成
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

    // 同步模式和网链模式都要"全员就绪才能播":
    //   同步模式 = 每个人都把本地文件加载好
    //   网链模式 = 每个人各自把链接解析/下载好（可能要等几分钟）
    // 只有共享模式不需要：视频是房主一路推过来的，观众没有"加载"这回事
    if (m_session->roomMode() == RoomSession::RoomMode::Share)
        return BlockReason::None;

    if (!m_playback->hasLoaded())
        return BlockReason::LocalNotLoaded;

    // 直播**不等人**:等所有人就绪，前面那段就错过了；而且各人拉流的延迟
    // 本来就有几秒差，等齐也没有意义。
    if (live())
        return BlockReason::None;

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
        // 卸载/换源/播完也会走到这里，但那是播放器生命周期，不是用户按了暂停。
        // 广播出去会把房间里的其他人一起按停。
        if (m_playback->lifecycleStop()) {
            LOG_DEBUG("Playback") << "生命周期停止(卸载/换源/播完)，不广播 Pause";
            return;
        }
        m_session->sendPlayback(RoomSession::PlaybackAction::Pause,
                                m_playback->position());
        return;
    }

    if (!canPlay()) {
        // 门禁拦截：退回暂停。注意这里**不广播** ——
        // 广播出去会把别人也按停，而他本来就不该被这条操作影响
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
    // 模式变化、进出房间：媒体源可能随之变化，走完整重算。
    handleRoomState(true);
}

void PlaybackSync::onMembersChanged() {
    if (m_session == nullptr || m_playback == nullptr)
        return;

    // 成员表刷新大部分时候只是加入/离开/状态回显，不影响媒体源。
    // 只有在共享模式下房主身份变化时，才需要重新决定“本地解码还是看远端”。
    const bool hostChanged = m_session->isHost() != m_lastIsHost;
    handleRoomState(hostChanged);
}

void PlaybackSync::handleRoomState(bool reloadMedia) {
    if (m_session == nullptr || m_playback == nullptr)
        return;

    m_lastIsHost = m_session->isHost();

    // 换房/退房时，上一条目的下载、失败、就绪状态都不再属于当前房间。
    // 必须在后面的 applyPlaylistItem 之前清，否则新房间可能复用旧标记。
    if (m_session->roomId() != m_roomId) {
        m_roomId = m_session->roomId();
        if (m_urlDownloader != nullptr)
            m_urlDownloader->cancel();
        clearDownloadItem();
        clearPrepared();
        clearAllFailedItems();
    }

    // 该显示哪一路画面：
    //
    //   同步模式          → 自己的画面（每人各自加载各自的文件）
    //   网链模式          → 自己的画面（大家各自播自己那份，没有"推流方"）
    //   共享模式 + 房主   → 自己的画面（房主就是推流的那一方）
    //   共享模式 + 非房主 → 远端画面（房主推过来的）
    //
    // 网链模式**不能**按"非房主就看远端"处理：那样观众会去等房主的推流，
    // 而这条模式的语义是"每个人自己从链接拿内容"。
    // 离开房间时一律回到“本地画面”（即使上一刻还是共享观众）：
    // 远端渲染器会因此停手，减少 pop 页面后它还往已销毁的 QVideoSink 写帧的窗口。
    m_showRemote = m_session->inRoom()
                   && m_session->roomMode() == RoomSession::RoomMode::Share
                   && !m_session->isHost();

    m_playback->setShowingRemote(m_showRemote);

    // 共享模式不支持倍速；离开房间也回到原速。只让房主广播 1.0，
    // 其它人本地复位即可，避免每个客户端各发一条位置略有差异的换挡命令。
    if (!m_session->inRoom()
        || m_session->roomMode() == RoomSession::RoomMode::Share) {
        resetPlaybackRate(m_session->inRoom() && m_session->isHost());
    }

    // 观众端的时长来自房主的 video_status(成员表里就有)
    m_syncedDuration = hostDuration();

    // 不在房间里了：把这一轮下载下来的东西清掉。
    // 会话内是保留的（切条目只停下载、不删文件，切回来直接复用），清理发生
    // 在「离开房间」——实际唯一会走到的一次是退出软件时 main 显式调
    // SessionController::leaveRoom()。
    //
    // 注意这里拿不到 roomId：RoomSession::leaveRoom() 先 clearRoomState()
    // 再发 left()，信号到达时房间号已被清空。所以删的是整个 clientId 目录，
    // 见 UrlDownloader::removeClientTempDir。
    if (!m_session->inRoom()) {
        // 先把在飞的弹幕请求掐掉，再删临时目录，避免回调晚一步往里写
        if (m_danmaku != nullptr)
            m_danmaku->reset();
        if (m_urlDownloader != nullptr) {
            m_urlDownloader->cancel();
            UrlDownloader::removeClientTempDir(m_session->clientId());
        }
        clearDownloadItem();
        clearPrepared();
        clearAllFailedItems();

        // 离开房间时不要再走 applyPlaylistItem()：RoomSession::clearRoomState()
        // 先清成员表、后清模式/播放列表，这段中间状态仍会看到旧的 Url 模式和
        // 当前条目，继续解析会白起一次 yt-dlp（日志里表现为“开始解析”后立刻
        // “已取消”）。播放器在这里显式卸载，避免离开房间后还占着文件。
        m_playback->unload();
        emit durationChanged();
        emit seekableChanged();
        emit positionChanged();
        emit playingChanged();
        emit gateChanged();
        emit downloadStateChanged();
        return;
    }

    // 非网链模式不再需要下载器：切走时把还在跑的解析/下载停掉。
    // 只取消，不删已下好的缓存（切回网链模式还能复用）。
    if (m_session->roomMode() != RoomSession::RoomMode::Url
        && !m_downloadItemId.isEmpty()) {
        cancelOngoingDownload();
    }
    if (m_session->roomMode() != RoomSession::RoomMode::Url)
        clearAllFailedItems();

    // 模式变了，"这一条从哪儿来"也跟着变：同步/共享读本地文件，网链读 URL。
    // 重新解析一次当前条目（没有当前条目时是空串，等价于卸载）。
    // 成员表刷新且房主身份未变时 reloadMedia=false：不要碰播放器。
    if (reloadMedia)
        applyPlaylistItem(m_session->playlist()->currentItemId());

    emit durationChanged();
    emit seekableChanged();
    emit positionChanged();
    emit playingChanged();
    emit gateChanged();
    // 当前条目变了，下载进度/错误的“是否属于当前条目”也会变
    emit downloadStateChanged();
    // retryingItemId 也按“是否为当前条目”计算，切条目后要重算
    emit retryStateChanged();
}

void PlaybackSync::onPlaylistSwitched(const QString &itemId) {
    if (m_session == nullptr || m_playback == nullptr)
        return;

    // 失败状态不在切条目时清除：切回失败条目仍然显示错误，必须由用户
    // 点刷新图标才会重新下载。离开房间/切到非网链模式时才清（见 onRoomChanged）。

    // 切集不自动播：位置归零、停在暂停，由用户按播放（和"换了部片子"的直觉一致）
    m_syncedPosition = 0;
    m_syncedPlaying = false;
    emit positionChanged();
    emit playingChanged();

    applyPlaylistItem(itemId);
    // 当前条目变了：即使媒体源没变（两个条目指向同一文件时 load() 直接返回），
    // 也要把 loaded 状态重新挂到新条目上，否则服务端只能看到上一条的旧状态。
    pushVideoStatus();
    emit liveChanged();
    emit gateChanged();
    // 切到一条正在后台下载的条目时，让进度文案立刻显示出来
    emit downloadStateChanged();
    // retryingItemId 从“后台”变成“当前”，或反过来
    emit retryStateChanged();

    // 直播没有共同时间轴，播放器对直播倍速的支持也不可靠：切到直播即回原速
    if (live())
        resetPlaybackRate(m_session->inRoom() && m_session->isHost());
}

void PlaybackSync::onPlaylistItemRemoved(const QString &itemId, bool wasCurrent) {
    if (m_session == nullptr || m_playback == nullptr || itemId.isEmpty())
        return;

    const bool wasDownloading = m_downloadItemId == itemId;
    const bool wasPrepared = m_preparedItemId == itemId;
    const bool wasFailed = m_failedItems.contains(itemId);

    if (!wasDownloading && !wasPrepared && !wasFailed && !wasCurrent)
        return;

    // 不管是不是当前条目，只要下载器正在处理它，就立即终止解析/探测/下载/合并。
    if (wasDownloading) {
        if (m_urlDownloader != nullptr)
            m_urlDownloader->cancel();
        clearDownloadItem();
        LOG_INFO("Playback") << "条目已删除，终止解析/下载:" << itemId;
    }

    if (wasPrepared)
        clearPrepared();

    if (wasFailed) {
        m_failedItems.remove(itemId);
        emit downloadStateChanged();
    }

    if (wasCurrent) {
        // 当前条目被删：播放器/弹幕立即退出；服务端随后会发 playlist_switched，
        // 由 applyPlaylistItem() 加载新的当前条目（列表空了就保持卸载）。
        if (m_danmaku != nullptr)
            m_danmaku->reset();
        m_playback->unload();
        emit gateChanged();
        emit durationChanged();
        emit liveChanged();
    }

    emit downloadStateChanged();
    emit retryStateChanged();
}

void PlaybackSync::applyPlaylistItem(const QString &itemId) {
    if (itemId.isEmpty()) {
        // 列表空了：把播放器卸载掉，画面回到"请先加载视频"
        if (m_danmaku != nullptr)
            m_danmaku->reset();
        clearPrepared();
        m_playback->unload();
        return;
    }

    const RoomSession::RoomMode mode = m_session->roomMode();

    // 共享模式的观众不加载任何东西 —— 画面是房主推过来的，
    // 本地文件在他们机器上根本不存在
    if (mode == RoomSession::RoomMode::Share && !m_session->isHost()) {
        if (m_danmaku != nullptr)
            m_danmaku->reset();
        clearPrepared();
        m_playback->unload();
        return;
    }

    if (mode == RoomSession::RoomMode::Url) {
        prepareUrlItem(itemId);
        return;
    }

    // 同步模式 / 共享模式的房主：本地文件，没有 B 站弹幕。
    // reset() 是幂等的，房间成员刷新时反复调用无副作用。
    if (m_danmaku != nullptr)
        m_danmaku->reset();

    // 本地模式 / 共享模式的房主：读本机文件映射
    const QString path = m_session->localFileFor(itemId);
    if (path.isEmpty()) {
        // 还没给这一条指定本机文件 —— 清空播放器，门禁会挡住播放，
        // 界面提示"请选择文件"
        clearPrepared();
        m_playback->unload();
        return;
    }

    m_playback->load(QUrl::fromLocalFile(path));
}

// ============================
// 网链模式：把一个链接变成能播的媒体
// ============================

void PlaybackSync::prepareUrlItem(const QString &itemId)
{
    const QString url = m_session->urlFor(itemId);
    if (url.isEmpty()) {
        if (m_danmaku != nullptr)
            m_danmaku->reset();
        clearPrepared();
        m_playback->unload();
        return;
    }

    // 切到另一条时，清掉上一条遗留的 failed/ready 标记；真正在跑的任务
    // （probing/downloading/merging/checking）不动，后台重试要靠它。
    if (!m_downloadItemId.isEmpty() && m_downloadItemId != itemId
        && m_urlDownloader != nullptr) {
        const QString stage = m_urlDownloader->stage();
        const bool active = stage == QLatin1String("probing")
                            || stage == QLatin1String("downloading")
                            || stage == QLatin1String("merging")
                            || stage == QLatin1String("checking");
        if (!active)
            clearDownloadItem();
    }

    // 已经在播放器里：不重启。必须同时确认实际 source 匹配，
    // 否则“网链 → 本地 → 网链”往返时会把本地文件误认为网链缓存。
    if (preparedSourceLoaded(itemId))
        return;

    // 当前条目已经失败：保留失败状态给界面显示，等用户点重试。
    // 否则每次房间状态刷新（video_status 回显等）都会自动重拉一次 yt-dlp，
    // 既刷屏又会把别的条目的后台重试任务挤掉。
    // 走到这里说明 preparedSourceLoaded() 已经是 false，播放器里不是它的就绪源。
    if (m_failedItems.contains(itemId)) {
        clearPrepared();
        m_playback->unload();
        return;
    }

    // ---- 下载器任务幂等保护 ----
    // 房间状态变一次就会走到这里一次(成员表更新、模式广播、连自己上报的
    // video_status 回显也算),不挡住的话会反复重启 yt-dlp。
    // 这里用 m_downloadItemId 而不是“当前条目”：后台重试另一条时，
    // 当前条目的 membersChanged 刷新不应该把那个任务推倒重来。
    if (itemId == m_downloadItemId && m_urlDownloader != nullptr) {
        const QString stage = m_urlDownloader->stage();
        if (stage == QLatin1String("probing")
            || stage == QLatin1String("resolved")   // 直链就绪,等 PlaybackSync 切播放器
            || stage == QLatin1String("downloading")
            || stage == QLatin1String("merging")
            || stage == QLatin1String("checking")) {
            // 用户刚切到一条正在后台下载的条目：播放器先清空等 ready；
            // 任务本身不重启。
            clearPrepared();
            m_playback->unload();
            return;
        }
    }

    // 弹幕与视频同目录、同生命周期。beginItem 对同一目录的在飞请求是幂等的，
    // 不会因为成员表刷新而反复 abort 重来。
    const QString dir = UrlDownloader::itemTempDir(
        m_session->roomId(), m_session->clientId(), itemId);
    const bool danmakuCached =
        m_danmaku != nullptr && m_danmaku->beginItem(dir, url);

    // 这一条之前下过了（会话内缓存）:直接放，不再解析下载。
    // 缓存文件只在这两种时候消失：退出软件时整体清掉(见 PlaybackSync 的
    // onSessionChanged),或者用户手点重试重新下载（那时会覆盖同一目录）。
    const QString cached = m_session->playlist()->localFile(itemId);
    if (!cached.isEmpty() && QFileInfo::exists(cached)) {
        LOG_INFO("Playback") << "复用已下载的缓存:" << cached;
        // 弹幕 XML 没命中缓存时，用会话内 cid / 页面 URL 补拉一次
        if (!danmakuCached && m_danmaku != nullptr)
            m_danmaku->loadFromUrl();
        // 这一条自己的下载任务已经结束；别的条目的后台任务不要动
        if (m_downloadItemId == itemId)
            clearDownloadItem();
        // 缓存能加载成功，说明之前的失败已经无关紧要
        if (m_failedItems.contains(itemId))
            clearFailedItem(itemId);
        const QUrl cachedSource = QUrl::fromLocalFile(cached);
        markPrepared(itemId, cachedSource);
        m_playback->load(cachedSource);
        return;
    }

    // 换了条目：只把还在跑的下载停掉，**不删文件** —— 下好的那份留着，
    // 切回来直接复用（以前这里是"切走就删",下过的片子每次都要重下）。
    // 后台重试另一个条目的任务也会在这里被取消：当前播放的条目优先。
    cancelOngoingDownload();
    m_downloadItemId = itemId;
    emit retryStateChanged();
    emit downloadStateChanged();

    // 准备期间播放器是空的 —— 门禁会自动挡住播放，界面显示"准备中"
    m_playback->unload();

    if (m_urlDownloader == nullptr) {
        // 没有下载器就退回直连（能播就播，不能播会走播放器的错误路径）
        if (m_danmaku != nullptr)
            m_danmaku->loadFromUrl();
        m_playback->load(QUrl(url));
        return;
    }

    // 按条目上记的 spec 解析（见 FormatModel.h）:选了什么就下什么，
    // 各端拿同一条 spec 各自解析，内容才对得上
    const QString spec = m_session->playlist()->specOf(itemId);
    LOG_INFO("Playback") << "网链模式:开始解析" << url
                         << QStringLiteral("spec[%1]").arg(spec)
                         << QStringLiteral("→")
                         << StreamSpec::label(spec, MediaInfo());
    // 下载合并要落盘，目录按条目算（clientId 不能少，见 itemTempDir 的说明）
    m_urlDownloader->setDownloadDir(dir);
    m_urlDownloader->start(url, spec);
}

void PlaybackSync::onUrlResolved()
{
    if (m_urlDownloader == nullptr || m_session == nullptr)
        return;

    // 下载器当前任务对应的条目。后台重试一个非当前条目时，它 != current。
    const QString itemId = m_downloadItemId;
    if (itemId.isEmpty()) {
        LOG_DEBUG("Playback") << "解析结果已过期,丢弃";
        return;
    }

    // 只有"直链可播"才会走到这个信号（分离流会自己进下载，见 UrlDownloader::start）
    if (!m_urlDownloader->direct())
        return;

    const bool isCurrent = itemId == m_session->playlist()->currentItemId();
    if (!isCurrent) {
        // 后台重试拿到直链：没有文件可缓存，切到该条目时重新解析即可。
        LOG_INFO("Playback") << "后台重试得到直链,无法缓存:" << itemId;
        clearDownloadItem();
        emit backgroundRetryFinished(
            itemId, true,
            QStringLiteral("该条目是直链，切换到该条目即可播放"));
        return;
    }

    // 路径 A:选中的是音视频合一格式（或直播的 m3u8）—— 直链交给播放器：
    // 秒开、能拖、不落盘
    LOG_INFO("Playback") << "直链播放:" << m_urlDownloader->title();
    // 记成"已就绪":直链这条路没有临时文件，但同样不该被反复重新解析
    const QUrl directSource(m_urlDownloader->directUrl());
    markPrepared(itemId, directSource);
    m_playback->load(directSource);
    clearFailedItem(itemId);
    clearDownloadItem();
}

void PlaybackSync::onUrlProbed()
{
    if (m_danmaku == nullptr || m_urlDownloader == nullptr || m_session == nullptr)
        return;
    if (!m_session->inRoom()
        || m_session->roomMode() != RoomSession::RoomMode::Url)
        return;

    // 只有当前条目的探测结果才用于弹幕；后台重试另一条时不要动当前弹幕。
    if (!downloadIsCurrent()) {
        LOG_DEBUG("Playback") << "探测结果不是当前条目,跳过弹幕";
        return;
    }

    // 与视频下载并行：XML 很小，通常远早于视频下完
    m_danmaku->loadFor(m_urlDownloader->mediaInfo());
}

void PlaybackSync::onUrlReady(const QString &filePath)
{
    if (m_session == nullptr) {
        QFile::remove(filePath);
        return;
    }

    const QString itemId = m_downloadItemId;
    if (itemId.isEmpty()) {
        LOG_DEBUG("Playback") << "下载完成但任务已作废,丢弃并删除:" << filePath;
        QFile::remove(filePath);
        return;
    }

    // 记下这份缓存属于哪一条：切走再切回来时靠它直接复用，不必重新解析下载。
    // 后台重试的条目也走这里，只是不加载播放器。
    m_session->playlist()->setLocalFile(itemId, filePath);

    const bool isCurrent = itemId == m_session->playlist()->currentItemId();
    if (isCurrent) {
        const QUrl readySource = QUrl::fromLocalFile(filePath);
        markPrepared(itemId, readySource);
        m_playback->load(readySource);
    } else {
        LOG_INFO("Playback") << "后台重试完成,已写入缓存:" << itemId << filePath;
    }

    if (m_failedItems.contains(itemId))
        clearFailedItem(itemId);
    clearDownloadItem();
    if (!isCurrent)
        emit backgroundRetryFinished(itemId, true, QString());
}

void PlaybackSync::onUrlFailed(const QString &message)
{
    const QString itemId = m_downloadItemId;

    // 当前条目的失败按条目记下来：即使随后有别的条目的后台重试覆盖了
    // m_downloadItemId，当前行的错误/重试状态也要保留。
    if (!itemId.isEmpty() && m_session != nullptr
        && itemId == m_session->playlist()->currentItemId()) {
        m_failedItems.insert(itemId, message);
        emit downloadStateChanged();
        return;
    }

    emit downloadStateChanged();
    if (itemId.isEmpty() || m_session == nullptr)
        return;

    LOG_WARN("Playback") << "后台重试失败:" << itemId << message;
    // 按条目记下失败原因：用户切到它时仍然显示错误，且不会自动重拉。
    m_failedItems.insert(itemId, message);
    clearDownloadItem();
    emit backgroundRetryFinished(itemId, false, message);
}

void PlaybackSync::cancelOngoingDownload()
{
    if (m_urlDownloader == nullptr)
        return;
    // **只停进程，不删文件** —— 下载好的那份留着给下次复用。
    // 删文件的时机只有两个：退出软件（整目录清掉）、重试（覆盖同一目录）
    m_urlDownloader->cancel();
    clearDownloadItem();
}

void PlaybackSync::onPositionTimer() {
    if (m_session == nullptr || m_playback == nullptr)
        return;

    // 只有"推流方"需要周期广播位置；观众端据此画进度条。
    // 其余时刻这个定时器空转，开销可忽略。
    if (!m_session->inRoom() || !m_session->isHost())
        return;
    if (m_session->roomMode() == RoomSession::RoomMode::Sync)
        return;
    if (screenSharing())
        return;
    if (live())
        return;   // 直播没有共同时间轴:位置广播和纠偏都没有意义
    if (!m_playback->playing())
        return;

    m_session->sendPlaybackPosition(m_playback->position(), true);
}

void PlaybackSync::pushVideoStatus() {
    if (m_session == nullptr || m_playback == nullptr || !m_session->inRoom())
        return;

    // 网链模式上报的是**来源身份**(链接 + 画质),不是文件字节的哈希：
    // 大家各自下载，CDN 变体和封装细节都可能不同，拿文件哈希去比会误报"不同步"。
    // 时长照报，服务端还能靠"时长差 > 2 秒"发现各人拿到的不是同一个版本。
    QString sourceHash;
    if (m_session->roomMode() == RoomSession::RoomMode::Url) {
        const QString itemId = m_session->playlist()->currentItemId();
        // spec 要拼进去：各人按同一条 spec 各自下载，spec 不同就是不同的内容，
        // 必须算成不同的来源身份——否则「一方下 720P、另一方下 1080P」会被判为已匹配。
        // 直播照旧只拼 #live(清晰度由清单自己协商，各端可能不一样)。
        const QString specTag = m_session->playlist()->currentIsLive()
                                    ? QStringLiteral("#live")
                                    : QStringLiteral("#") + m_session->playlist()->specOf(itemId);
        sourceHash = MediaHash::forText(m_session->urlFor(itemId) + specTag);
    }

    m_session->reportVideoStatus(m_playback->hasLoaded(),
                                 sourceHash.isEmpty() ? m_playback->hash()
                                                      : sourceHash,
                                 m_playback->duration());
}

// ============================
// 房间 -> 本地 / 同步状态
// ============================

void PlaybackSync::onRemoteCommand(RoomSession::PlaybackAction action, qint64 position,
                                   qreal rate) {
    if (m_session == nullptr || m_playback == nullptr)
        return;

    // 屏幕共享没有共同电影时间轴。观众端不更新同步状态，房主端不执行
    // 电影操作，避免共享期间有人按播放/拖动把房主的电影唤醒。
    if (screenSharing())
        return;

    rate = qBound(0.25, rate, 4.0);

    // 观众端：命令是"状态更新",不是驱动本地播放器（观众没有媒体）
    if (m_showRemote) {
        if (action == RoomSession::PlaybackAction::Rate) {
            // 共享模式的观众没有本地播放器，倍速只用于同步状态（界面在
            // 共享模式下本来就藏起了倍速入口，这里保持状态一致而已）
            if (!qFuzzyCompare(m_syncedRate, rate)) {
                m_syncedRate = rate;
                emit playbackRateChanged();
            }
            return;
        }

        // seek 后本地音频环形缓冲和 NetEq 里还有旧声音，视频队列里也有旧帧；
        // 让主函数清一次，避免先听到/看到 seek 前的内容。
        if (action == RoomSession::PlaybackAction::Seek && position >= 0)
            emit remoteSeeked(position);

        updateSyncedState(position, action == RoomSession::PlaybackAction::Play);
        return;
    }

    const RemoteScope scope(m_applyingRemote);

    if (action == RoomSession::PlaybackAction::Rate) {
        // 换挡命令带位置，但只有在明显对不上时才 seek —— 换挡本身不该让画面卡一下。
        // 同步模式没有周期性纠偏，所以这个对齐比其它模式更重要。
        // position < 0 表示发送方没有已加载的媒体，此时只换挡、不对齐。
        if (position >= 0 && !live() && m_playback->hasLoaded()
            && qAbs(m_playback->position() - position) > 500)
            m_playback->seek(position);

        m_playback->setPlaybackRate(rate);
        return;
    }

    // 其余动作都先对齐位置，再执行动作。

    if (!live())
        m_playback->seek(position);

    switch (action) {
    case RoomSession::PlaybackAction::Play:
        // 本地模式下还有人没加载完就先不播，停在原地等
        if (canPlay())
            m_playback->play();
        break;

    case RoomSession::PlaybackAction::Pause:
        m_playback->pause();
        break;

    case RoomSession::PlaybackAction::Seek:
    case RoomSession::PlaybackAction::Rate:
        break;
    }
}

void PlaybackSync::resetPlaybackRate(bool broadcast) {
    const bool localChanged = m_playback != nullptr
                              && !qFuzzyCompare(m_playback->playbackRate(), 1.0);

    if (localChanged)
        m_playback->setPlaybackRate(1.0);

    const bool syncedChanged = !qFuzzyCompare(m_syncedRate, 1.0);
    if (syncedChanged) {
        m_syncedRate = 1.0;
        emit playbackRateChanged();
    }

    if (broadcast && m_session != nullptr && m_session->inRoom()
        && (localChanged || syncedChanged)) {
        const qint64 ratePosition = (m_playback != nullptr && m_playback->hasLoaded())
                                        ? m_playback->position()
                                        : -1;
        m_session->sendPlayback(RoomSession::PlaybackAction::Rate,
                                ratePosition, 1.0);
    }
}

void PlaybackSync::onPositionUpdate(qint64 position, bool playing) {
    // 同步模式不进行纠偏

    if (m_showRemote) {
        updateSyncedState(position, playing);
        return;
    }

    if (m_session != nullptr && m_playback != nullptr
        && m_session->roomMode() == RoomSession::RoomMode::Url
        && !live()   // 直播不纠偏:没有共同时间轴
        && !m_session->isHost() && playing && m_playback->hasLoaded()) {
        const qint64 drift = qAbs(m_playback->position() - position);
        if (drift > 3000) {
            LOG_INFO("Playback") << "网链模式纠偏:与房主差" << drift / 1000
                                 << "秒,跳转对齐";
            const RemoteScope scope(m_applyingRemote);
            m_playback->seek(position);
        }
    }
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

    // 成员数只有个位数，线性扫描足够
    for (const Member &member : m_session->members()->members()) {
        if (member.isHost)
            return member.duration;
    }
    return 0;
}
