//
// 网链模式：把一个条目（spec 决定的那些流）转换为可播放的媒体。
//
// 两条路径，由 FormatModel::resolve(spec) 的结果决定：
//   A. 直链流播 —— 选中音视频合一的格式（或直播 m3u8）时把地址直接交给
//      QMediaPlayer，可秒开、可拖动进度、不落盘。高清基本都是分离的，
//      合一仅见于低清，故这条路主要出现在用户手选「合并格式」或站点只提供
//      单文件格式时。
//   B. 下载合并 —— 画面与声音是两条流（DASH），须下载完再由 ffmpeg 合并。
//      该路径无法边下边播：mp4 的索引（moov）位于文件尾，未下载完的文件打不开，
//      且合并本身需要先落盘。
//
// 挑选规则全部在 FormatModel（纯逻辑、可独立验证），本类只负责进程、进度与文件。
// 画质以 spec 字符串记在条目上（见 FormatModel.h 顶部），各端按同一条 spec 各自
// 解析下载，从而保证「选了什么」是各端共同认可的，内容才能对上。
// 本类只做解析与下载，不涉及播放器与房间，那些属于 PlaybackSync。
//

#ifndef SYNCINE_URLDOWNLOADER_H
#define SYNCINE_URLDOWNLOADER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "media/FormatModel.h"

class QNetworkAccessManager;
class QNetworkReply;

class QProcess;
class CookieManager;
class Toolchain;

class UrlDownloader : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString stage
               READ stage
               NOTIFY stateChanged)

    // 0 ~ 100;-1 = 总大小未知（直播流或没给 Content-Length）
    Q_PROPERTY(int progress
               READ progress
               NOTIFY stateChanged)

    // 给界面看的一行小字："8.2MB/s · 剩 1:20"
    Q_PROPERTY(QString detailText
               READ detailText
               NOTIFY stateChanged)

    Q_PROPERTY(QString title
               READ title
               NOTIFY resolved)

    Q_PROPERTY(qint64 durationMs
               READ durationMs
               NOTIFY resolved)

    // 失败原因：yt-dlp 的原始输出（它通常直接说清是缺 cookie 还是版本太旧）
    Q_PROPERTY(QString errorText
               READ errorText
               NOTIFY failed)

    // 是不是直播源（yt-dlp 的 is_live）。直播没有时长，而且**不能走下载合并** ——
    // 没有"下完"这一刻。
    Q_PROPERTY(bool live
               READ live
               NOTIFY resolved)

    // 选中的流带不带 HDR:Qt 播放器放 HDR 要过一遍离线转 SDR(见 HdrConverter),
    // 界面靠这个提前提醒
    Q_PROPERTY(bool hdr
               READ hdr
               NOTIFY resolved)

public:
    // cookies 可以为空（那就不带 --cookies）
    explicit UrlDownloader(const Toolchain *toolchain,
                           const CookieManager *cookies = nullptr,
                           QObject *parent = nullptr);
    ~UrlDownloader() override;

    // 解析链接并**按 spec 定下要下的流**,然后发起验证/下载，二选一地走完：
    //   · 选中合一格式 → 直链试拉（最多 8 秒）→ 发出 resolved(),direct = true
    //   · 选中分离流   → 直接开始下载合并（阶段 "downloading"）,完成发 ready()
    // 失败发 failed()。spec 语义见 FormatModel.h(空 = 最高画质)。
    Q_INVOKABLE void start(const QString &url, const QString &spec);

    // 下载合并的落盘目录（必须在 start() 之前设好）。
    // 为什么和 start() 分开：直链那条路不落盘，目录是上层按条目算的
    // (见 itemTempDir 的说明),不该混进 URL/spec 这两个跨端数据里。
    void setDownloadDir(const QString &dir) { m_destDir = dir; }

    // 取消当前任务，**并把状态清回干净**(没有任务在跑时也一样清)。
    // 界面关弹窗时靠它复位；只发 stateChanged,不会发 resolved/failed。
    Q_INVOKABLE void cancel();

    // 由上层（PlaybackSync）报告"这条准备不了"的原因 —— 比如直播源没有
    // 可直接播放的流。走同一条状态通道，界面不用多认一个字段。
    void reportFailure(const QString &message);

    QString stage() const { return m_stage; }
    int progress() const { return m_progress; }
    QString detailText() const { return m_detail; }
    QString title() const { return m_media.title; }
    qint64 durationMs() const { return m_media.durationMs; }
    QString errorText() const { return m_error; }
    bool live() const { return m_media.live; }
    bool hdr() const { return m_selection.hdr; }

    // 解析结果：选中了哪条流、是直链还是下载合并
    const MediaInfo &mediaInfo() const { return m_media; }
    const MediaSelection &selection() const { return m_selection; }
    // 选中的是合一格式（或直播）:地址可以直接交给播放器
    bool direct() const { return m_selection.valid && m_selection.direct; }
    QString directUrl() const { return m_selection.url; }

    // 临时目录。下载好的文件**会话内保留**(切条目只停下载、不删文件，
    // 切回来直接复用),一个房间的整个目录在**退出软件时**清掉一次
    // (清理入口见 removeRoomTempDir 的说明)。
    //
    // **路径里必须带 clientId**:同一台机器上开两个客户端测试是常态，
    // 而房间号 + 条目 id 对两个客户端是一样的 —— 不带 clientId 的话
    // 两个 yt-dlp 会往同一个目录里写，互相把对方的中间文件改名/删掉，
    // 报出 "Unable to rename file ... ENOENT",还会留下半截的流文件。
    static QString itemTempDir(const QString &roomId, const QString &clientId,
                               const QString &itemId);
    // 清掉本客户端的整个临时目录：<临时目录>/syncine/<clientId>/。
    // **只在退出软件时调用**（由主函数在事件循环退出后经
    // SessionController::leaveRoom → PlaybackSync 触发）。
    //
    // 为什么不按房间删：走到这里时房间号已经拿不到了 ——
    // RoomSession::leaveRoom() 是先 clearRoomState()（里面 m_roomId.clear()）
    // 再 emit left()，所以信号到达时 roomId 是空串。删整个 clientId 目录
    // 既覆盖当前房间，也顺手清掉上一次运行残留的目录。
    static void removeClientTempDir(const QString &clientId);

signals:
    void stateChanged();
    // 解析完成
    void probed();
    // 直链验证通过，可以交给播放器（仅直链路径；下载合并路径走 ready()）
    void resolved();
    void ready(const QString &filePath);
    void failed(const QString &message);

private:
    // 探测 + 按 spec 解析：决定了"直链"还是"下载合并"之后自动接着走
    void startResolve(const QString &url, const QString &spec);
    void handleResolveFinished(int exitCode);
    // 真的去试一下这个直链能不能拉（不带任何自定义头，和播放器处境一致）
    void verifyDirectUrl(const QString &url);
    // 掐掉还在飞的直链试拉，免得它的回调晚一步把已经作废的结果当成新的
    void abortVerify();
    void beginDownload(int exitCode);
    // 起一个 yt-dlp:options 会在 URL **前面**,并自动补上 --ffmpeg-location
    void startProcess(const QStringList &options, const QString &url);
    void handleDownloadLine(const QString &line);
    void handleDownloadFinished(int exitCode);
    void setStage(const QString &stage);
    void fail(const QString &message);

    const Toolchain *m_toolchain = nullptr;
    // 有 cookie 文件时给 yt-dlp 带上 --cookies(见 CookieManager::cookieArgs)
    const CookieManager *m_cookies = nullptr;
    QProcess *m_process = nullptr;

    QString m_stage = QStringLiteral("idle");
    int m_progress = -1;
    QString m_detail;
    QString m_error;

    // 解析结果
    QString m_url;              // 正在处理的链接(下载时还要再用一次)
    QString m_spec;             // 条目上的 spec(见 FormatModel.h)
    MediaInfo m_media;          // 探测到的全貌
    MediaSelection m_selection; // 按 spec 选中的流

    // 下载用
    QString m_destDir;
    QString m_outputTemplate;   // -o 模板(产物靠 glob 找,见 handleDownloadFinished)
    qint64 m_totalBytes = 0;
    // 分离流的整体进度：yt-dlp 先下画面再下声音，每条流的进度都从 0 重新计数。
    // 用「已完成的流 + 当前流」折算整体百分比，否则进度条会在换流时倒退。
    qint64 m_currentFileBytes = 0;
    qint64 m_finishedBytes = 0;
    qint64 m_expectedTotalBytes = 0;
    double m_speedBps = 0;
    int m_etaSec = -1;
    // 手选的精确流在本端不存在时，只允许退回档位重下一次（见 handleDownloadFinished）
    bool m_manualFallbackTried = false;

    // yt-dlp 的输出尾巴，失败时原样给用户看（最后几行足够定位问题）
    QStringList m_outputTail;
    // 直链验证（异步）
    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_verifyReply = nullptr;
    QString m_verifyCandidate;
    QString m_verifyRejectReason;
    // 探测时把整份输出攒起来（JSON 可能被拆成多次 readyRead 到达）
    QByteArray m_outputBuffer;
};

#endif //SYNCINE_URLDOWNLOADER_H
