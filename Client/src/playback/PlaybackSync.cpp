//
// Created by donggu on 2026/9/13.
//

#include "PlaybackSync.h"

#include <QFile>
#include <QFileInfo>

#include "core/Log.h"
#include "core/MediaHash.h"
#include "core/UrlDownloader.h"
#include "media/FormatModel.h"
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
                           UrlDownloader *urlDownloader,
                           QObject *parent)
    : QObject(parent),
      m_session(session),
      m_playback(playback),
      m_urlDownloader(urlDownloader) {

    if (m_session == nullptr || m_playback == nullptr)
        return;

    // ---- 网链模式:解析/下载的结果 ----
    if (m_urlDownloader != nullptr) {
        connect(m_urlDownloader, &UrlDownloader::resolved,
                this, &PlaybackSync::onUrlResolved);
        connect(m_urlDownloader, &UrlDownloader::ready,
                this, &PlaybackSync::onUrlReady);
        connect(m_urlDownloader, &UrlDownloader::stateChanged,
                this, &PlaybackSync::downloadStateChanged);
        connect(m_urlDownloader, &UrlDownloader::failed, this,
                [this](const QString &) { emit downloadStateChanged(); });
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

    // 房间的"当前条目"变了 —— 播放源由这里决定,是唯一会调用 load/unload 的地方
    connect(m_session, &RoomSession::playlistSwitched,
            this, &PlaybackSync::onPlaylistSwitched);
    connect(m_session, &RoomSession::entered,
            this, [this]() {
                // 进房之前可能就已经加载好视频了,补报一次,
                // 否则服务端不知道"这个成员能播"
                pushVideoStatus();
                onRoomChanged();
            });

    // ---- 直播掉线重连 ----
    // 这里自动重新解析一遍再加载,延迟 3 秒避免疯狂重试。
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

                    // 清掉"已就绪"标记,让 prepareUrlItem 重新走一遍解析
                    m_preparedItemId.clear();
                    m_preparingItemId.clear();
                    prepareUrlItem(m_session->playlist()->currentItemId());
                });
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

// 网链模式的准备进度。其它模式下下载器是空闲的,这几个值自然是空的。
QString PlaybackSync::downloadStage() const {
    return m_urlDownloader != nullptr ? m_urlDownloader->stage() : QString();
}

int PlaybackSync::downloadProgress() const {
    return m_urlDownloader != nullptr ? m_urlDownloader->progress() : -1;
}

QString PlaybackSync::downloadDetail() const {
    return m_urlDownloader != nullptr ? m_urlDownloader->detailText() : QString();
}

QString PlaybackSync::downloadError() const {
    return m_urlDownloader != nullptr ? m_urlDownloader->errorText() : QString();
}

bool PlaybackSync::live() const {
    if (m_session == nullptr)
        return false;
    return m_session->playlist()->currentIsLive();
}

QString PlaybackSync::downloadStageText() const
{
    if (m_urlDownloader == nullptr)
        return {};

    const QString stage = m_urlDownloader->stage();
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
    if (stage == QLatin1String("failed"))
        return QStringLiteral("下载失败");
    return {};
}

void PlaybackSync::retryCurrentItem() {
    if (m_session == nullptr)
        return;

    const QString itemId = m_session->playlist()->currentItemId();
    if (itemId.isEmpty())
        return;

    LOG_INFO("Playback") << "重试当前条目";
    // 先掐掉可能在跑的那次(失败后通常已经结束;取消是对"下载卡住"的兜底)
    if (m_urlDownloader != nullptr)
        m_urlDownloader->cancel();
    m_preparedItemId.clear();
    m_preparingItemId.clear();
    // "重试"的意思是**重新下一遍**:把缓存的映射删掉,否则 prepareUrlItem
    // 会认出上一轮的产物直接复用它(重试就变成空转)
    m_session->playlist()->setLocalFile(itemId, QString());
    prepareUrlItem(itemId);
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

    // 同步模式和网链模式都要"全员就绪才能播":
    //   同步模式 = 每个人都把本地文件加载好
    //   网链模式 = 每个人各自把链接解析/下载好(可能要等几分钟)
    // 只有共享模式不需要:视频是房主一路推过来的,观众没有"加载"这回事
    if (m_session->roomMode() == RoomSession::RoomMode::Share)
        return BlockReason::None;

    if (!m_playback->hasLoaded())
        return BlockReason::LocalNotLoaded;

    // 直播**不等人**:等所有人就绪,前面那段就错过了;而且各人拉流的延迟
    // 本来就有几秒差,等齐也没有意义。
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
    //   同步模式          → 自己的画面(每人各自加载各自的文件)
    //   网链模式          → 自己的画面(大家各自播自己那份,没有"推流方")
    //   共享模式 + 房主   → 自己的画面(房主就是推流的那一方)
    //   共享模式 + 非房主 → 远端画面(房主推过来的)
    //
    // 网链模式**不能**按"非房主就看远端"处理:那样观众会去等房主的推流,
    // 而这条模式的语义是"每个人自己从链接拿内容"。
    m_showRemote = (m_session->roomMode() == RoomSession::RoomMode::Share)
                   && !m_session->isHost();

    m_playback->setShowingRemote(m_showRemote);

    // 观众端的时长来自房主的 video_status(成员表里就有)
    m_syncedDuration = hostDuration();

    // 离开房间:临时文件不该留下(这里不做持久缓存)
    if (!m_session->inRoom()) {
        if (m_urlDownloader != nullptr) {
            m_urlDownloader->cancel();
            UrlDownloader::removeRoomTempDir(m_session->roomId(), m_session->clientId());
        }
        m_preparingItemId.clear();
        m_preparedItemId.clear();
    }

    // 模式变了,"这一条从哪儿来"也跟着变:同步/共享读本地文件,网链读 URL。
    // 重新解析一次当前条目(没有当前条目时是空串,等价于卸载)
    applyPlaylistItem(m_session->playlist()->currentItemId());

    emit durationChanged();
    emit seekableChanged();
    emit positionChanged();
    emit playingChanged();
    emit gateChanged();
}

void PlaybackSync::onPlaylistSwitched(const QString &itemId) {
    if (m_session == nullptr || m_playback == nullptr)
        return;


    // 切集不自动播:位置归零、停在暂停,由用户按播放(和"换了部片子"的直觉一致)
    m_syncedPosition = 0;
    m_syncedPlaying = false;
    emit positionChanged();
    emit playingChanged();

    applyPlaylistItem(itemId);
    emit liveChanged();
    emit gateChanged();
}

void PlaybackSync::applyPlaylistItem(const QString &itemId) {
    if (itemId.isEmpty()) {
        // 列表空了:把播放器卸载掉,画面回到"请先加载视频"
        m_playback->unload();
        return;
    }

    const RoomSession::RoomMode mode = m_session->roomMode();

    // 共享模式的观众不加载任何东西 —— 画面是房主推过来的,
    // 本地文件在他们机器上根本不存在
    if (mode == RoomSession::RoomMode::Share && !m_session->isHost()) {
        m_playback->unload();
        return;
    }

    if (mode == RoomSession::RoomMode::Url) {
        prepareUrlItem(itemId);
        return;
    }

    // 本地模式 / 共享模式的房主:读本机文件映射
    const QString path = m_session->localFileFor(itemId);
    if (path.isEmpty()) {
        // 还没给这一条指定本机文件 —— 清空播放器,门禁会挡住播放,
        // 界面提示"请选择文件"
        m_playback->unload();
        return;
    }

    m_playback->load(QUrl::fromLocalFile(path));
}

// ============================
// 网链模式:把一个链接变成能播的媒体
// ============================

void PlaybackSync::prepareUrlItem(const QString &itemId)
{
    const QString url = m_session->urlFor(itemId);
    if (url.isEmpty()) {
        m_playback->unload();
        return;
    }

    // ---- 幂等保护 ----
    // 房间状态变一次就会走到这里一次(成员表更新、模式广播、连自己上报的
    // video_status 回显也算),不挡住的话会反复重启 yt-dlp,
    // 而且每次都会把已经加载好的媒体卸载掉 —— 表现为永远"解析中"。
    if (itemId == m_preparingItemId && m_urlDownloader != nullptr) {
        const QString stage = m_urlDownloader->stage();
        if (stage == QLatin1String("probing")
            || stage == QLatin1String("resolved")   // 直链就绪,等 PlaybackSync 切播放器
            || stage == QLatin1String("downloading")
            || stage == QLatin1String("merging")) {
            return; // 正在准备(或已经准备好),别把它推倒重来
        }
    }
    if (itemId == m_preparedItemId && m_playback->hasLoaded())
        return; // 已经就绪,播放器上就是它

    // 这一条之前下过了(会话内缓存):直接放,不再解析下载。
    // 缓存文件只在这两种时候消失:退出软件时整体清掉(见 PlaybackSync 的
    // onSessionChanged),或者用户手点重试重新下载(那时会覆盖同一目录)。
    const QString cached = m_session->playlist()->localFile(itemId);
    if (!cached.isEmpty() && QFileInfo::exists(cached)) {
        LOG_INFO("Playback") << "复用已下载的缓存:" << cached;
        m_preparedItemId = itemId;
        m_playback->load(QUrl::fromLocalFile(cached));
        return;
    }

    // 换了条目:只把还在跑的下载停掉,**不删文件** —— 下好的那份留着,
    // 切回来直接复用(以前这里是"切走就删",下过的片子每次都要重下)
    cancelOngoingDownload();
    m_preparingItemId = itemId;

    // 准备期间播放器是空的 —— 门禁会自动挡住播放,界面显示"准备中"
    m_playback->unload();

    if (m_urlDownloader == nullptr) {
        // 没有下载器就退回直连(能播就播,不能播会走播放器的错误路径)
        m_playback->load(QUrl(url));
        return;
    }

    // 按条目上记的 spec 解析(见 FormatModel.h):选了什么就下什么,
    // 各端拿同一条 spec 各自解析,内容才对得上
    const QString spec = m_session->playlist()->specOf(itemId);
    LOG_INFO("Playback") << "网链模式:开始解析" << url
                         << QStringLiteral("spec[%1]").arg(spec)
                         << QStringLiteral("→")
                         << StreamSpec::label(spec, MediaInfo());
    // 下载合并要落盘,目录按条目算(clientId 不能少,见 itemTempDir 的说明)
    m_urlDownloader->setDownloadDir(
        UrlDownloader::itemTempDir(m_session->roomId(), m_session->clientId(), itemId));
    m_urlDownloader->start(url, spec);
}

void PlaybackSync::onUrlResolved()
{
    if (m_urlDownloader == nullptr)
        return;

    // 解析这几秒里用户可能已经切走了 —— 结果作废,别把播放器抢回来
    const QString itemId = m_session->playlist()->currentItemId();
    if (itemId.isEmpty() || itemId != m_preparingItemId) {
        LOG_DEBUG("Playback") << "解析结果已过期(条目换过了),丢弃";
        return;
    }

    // 只有"直链可播"才会走到这个信号(分离流会自己进下载,见 UrlDownloader::start)
    if (!m_urlDownloader->direct())
        return;

    // 路径 A:选中的是音视频合一格式(或直播的 m3u8)—— 直链交给播放器:
    // 秒开、能拖、不落盘
    LOG_INFO("Playback") << "直链播放:" << m_urlDownloader->title();
    // 记成"已就绪":直链这条路没有临时文件,但同样不该被反复重新解析
    m_preparedItemId = itemId;
    m_playback->load(QUrl(m_urlDownloader->directUrl()));
}

void PlaybackSync::onUrlReady(const QString &filePath)
{
    const QString itemId = m_session->playlist()->currentItemId();
    if (itemId.isEmpty() || itemId != m_preparingItemId) {
        LOG_DEBUG("Playback") << "下载完成但条目已经切走,丢弃并删除:" << filePath;
        QFile::remove(filePath);
        return;
    }

    // 记下这份缓存属于哪一条:切走再切回来时靠它直接复用,不必重新解析下载。
    // (会话内的映射;退出软件时缓存文件本身会被清掉,这份映射也随会话结束消失)
    m_session->playlist()->setLocalFile(itemId, filePath);

    m_preparedItemId = itemId;
    m_playback->load(QUrl::fromLocalFile(filePath));
}

void PlaybackSync::cancelOngoingDownload()
{
    if (m_urlDownloader == nullptr)
        return;
    // **只停进程,不删文件** —— 下载好的那份留着给下次复用。
    // 删文件的时机只有两个:退出软件(整目录清掉)、重试(覆盖同一目录)
    m_urlDownloader->cancel();
}

void PlaybackSync::onPositionTimer() {
    if (m_session == nullptr || m_playback == nullptr)
        return;

    // 只有"推流方"需要周期广播位置;观众端据此画进度条。
    // 其余时刻这个定时器空转,开销可忽略。
    if (!m_session->inRoom() || !m_session->isHost())
        return;
    if (m_session->roomMode() == RoomSession::RoomMode::Sync)
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

    // 网链模式上报的是**来源身份**(链接 + 画质),不是文件字节的哈希:
    // 大家各自下载,CDN 变体和封装细节都可能不同,拿文件哈希去比会误报"不同步"。
    // 时长照报,服务端还能靠"时长差 > 2 秒"发现各人拿到的不是同一个版本。
    QString sourceHash;
    if (m_session->roomMode() == RoomSession::RoomMode::Url) {
        const QString itemId = m_session->playlist()->currentItemId();
        // spec 要拼进去:各人按同一条 spec 各自下载,spec 不同就是不同的内容,
        // 必须算成不同的来源身份 —— 否则"我下 720P 你下 1080P"会被判成"已匹配"。
        // 直播照旧只拼 #live(清晰度由清单自己协商,各端可能不一样)。
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

void PlaybackSync::onRemoteCommand(RoomSession::PlaybackAction action, qint64 position) {
    if (m_session == nullptr || m_playback == nullptr)
        return;

    // 观众端:命令是"状态更新",不是驱动本地播放器(观众没有媒体)
    if (m_showRemote) {
        updateSyncedState(position, action == RoomSession::PlaybackAction::Play);
        return;
    }

    const RemoteScope scope(m_applyingRemote);

    // 三种动作都先对齐位置,再执行动作。

    if (!live())
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

    // 成员数只有个位数,线性扫描足够
    for (const Member &member : m_session->members()->members()) {
        if (member.isHost)
            return member.duration;
    }
    return 0;
}
