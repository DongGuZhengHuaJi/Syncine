//
// 网链模式:把"一个网页链接"变成"能播的媒体"。
//
// 两条路,由 probe() 的结果决定:
//
//   A. 直链流播 —— 站点提供**单文件格式**(画面和声音在同一个流里)时,
//      直接把直链交给 QMediaPlayer:秒开、可拖进度、不落盘。
//      多数站点的中低画质、以及直链 mp4 都属于这种。
//
//   B. 下载合并 —— 高画质基本都是 DASH(画面和声音两条流分开),
//      必须下完再由 ffmpeg 合并。**这条路上"边下边播"做不到**:
//      mp4 的索引(moov)在文件尾,没下完的文件打不开;而且合并本身要先落盘。
//
// 本类只管"探测"和"下载"两件事,不碰播放器、不管房间 —— 那些是 PlaybackSync 的事。
//
// 画质:档位用**视频高度上限**表示(1080 / 720 / 0=最高),由 probe/inspect 从
// formats 里归纳出来给用户挑。它最终会变成 yt-dlp 的格式表达式
// (`bv*[height<=720]+ba/b[height<=720]`)—— 也就是说各端**各自按同一上限下载**,
// 而不是下同一份文件。所以档位必须是**条目级**的(见 PlaylistEntry::quality):
// 一个人 720P 一个人 1080P,两边内容哈希必然对不上。
//
// 直链那条路要小心:高清基本都是音视频分离的(合一的只有低清),
// 所以**指定了档位就要求直链高度正好等于该档位**,否则宁可不直链 ——
// 不然"选 1080P"会悄悄变成"秒开 360P"。
//

#ifndef SYNCINE_URLDOWNLOADER_H
#define SYNCINE_URLDOWNLOADER_H

#include <QObject>
#include <QString>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;

class QProcess;
class Toolchain;

class UrlDownloader : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString stage
               READ stage
               NOTIFY stateChanged)

    // 0 ~ 100;-1 = 总大小未知(直播流或没给 Content-Length)
    Q_PROPERTY(int progress
               READ progress
               NOTIFY stateChanged)

    // 给界面看的一行小字:"8.2MB/s · 剩 1:20"
    Q_PROPERTY(QString detailText
               READ detailText
               NOTIFY stateChanged)

    Q_PROPERTY(QString title
               READ title
               NOTIFY probed)

    Q_PROPERTY(qint64 durationMs
               READ durationMs
               NOTIFY probed)

    // 失败原因:yt-dlp 的原始输出(它通常直接说清是缺 cookie 还是版本太旧)
    Q_PROPERTY(QString errorText
               READ errorText
               NOTIFY failed)

    // 是不是直播源(yt-dlp 的 is_live)。
    // 直播没有时长,而且**不能走下载合并** —— 没有"下完"这一刻。
    Q_PROPERTY(bool live
               READ live
               NOTIFY probed)

    // 上传者/频道名,给"加条目弹窗"显示一行来源信息用
    Q_PROPERTY(QString uploader
               READ uploader
               NOTIFY stateChanged)

    // 可选的画质档位:每项是 { label:"1080P", height:1080 } 的 map,
    // **第一项固定是 { label:"最高画质", height:0 }**。
    //
    // height 是"高度上限",0 表示不设限(和 PlaylistEntry::quality 同一套语义)。
    // 给界面看的是 label,真正往下传的是 height。
    //
    // 注意它是 QVariantList:QML 里当 model 用时**只有 modelData**,
    // 没有角色名(那是 QAbstractListModel 才有的)。
    Q_PROPERTY(QVariantList qualities
               READ qualities
               NOTIFY stateChanged)

public:
    explicit UrlDownloader(const Toolchain *toolchain, QObject *parent = nullptr);
    ~UrlDownloader() override;

    // 解析链接:拿标题/时长,并挑一个单文件格式作为直链候选。
    // 成功发 probed(),失败发 failed()。heightCap 见下面 inspect 的说明。
    //
    // 注意 heightCap 的默认值**只有 C++ 调用者能省**(moc 不把默认值写进
    // meta-object,QML 少传参数会直接报错),所以 QML 用的是 inspect()。
    Q_INVOKABLE void probe(const QString &url, int heightCap = 0);

    // 只"看一看"这个链接:拿标题/时长/上传者/有哪些画质档位,
    // **不挑直链、也不试拉**(那一步最多要等 8 秒,只是给人看一眼没必要)。
    // 给"加条目弹窗"用。
    Q_INVOKABLE void inspect(const QString &url);

    // 下载并按需合并到 destDir(会创建目录)。完成发 ready(filePath)。
    // heightCap > 0 时只取不超过这个高度的画面。
    Q_INVOKABLE void download(const QString &url, const QString &destDir,
                              int heightCap = 0);

    // 取消当前任务,**并把状态清回干净**(没有任务在跑时也一样清)。
    // 界面关弹窗时靠它复位;只发 stateChanged,不会发 probed/failed。
    Q_INVOKABLE void cancel();

    // 由上层(PlaybackSync)报告"这条准备不了"的原因 —— 比如直播源没有
    // 可直接播放的流。走同一条状态通道,界面不用多认一个字段。
    void reportFailure(const QString &message);

    // 取消,并且**等进程真正退出之后**再删这个目录。
    // 为什么不能"先 kill 再立刻删":kill 是异步的,而且 yt-dlp 还会拉起
    // ffmpeg 子进程 —— 目录被删掉时它们可能正往里写,于是重命名报
    // ENOENT(实测症状),留下半截文件。
    void cancelAndRemoveDir(const QString &dir);

    QString stage() const { return m_stage; }
    int progress() const { return m_progress; }
    QString detailText() const { return m_detail; }
    QString title() const { return m_title; }
    qint64 durationMs() const { return m_durationMs; }
    QString errorText() const { return m_error; }
    bool live() const { return m_isLive; }
    QString uploader() const { return m_uploader; }
    QVariantList qualities() const { return m_qualities; }

    // 探测挑出来的直链;空 = 这条链接只能走"下载合并"
    QString directUrl() const { return m_directUrl; }
    bool canStreamDirectly() const { return !m_directUrl.isEmpty(); }

    // 临时目录(不用持久缓存:切走/离开房间就清)。
    //
    // **路径里必须带 clientId**:同一台机器上开两个客户端测试是常态,
    // 而房间号 + 条目 id 对两个客户端是一样的 —— 不带 clientId 的话
    // 两个 yt-dlp 会往同一个目录里写,互相把对方的中间文件改名/删掉,
    // 报出 "Unable to rename file ... ENOENT",还会留下半截的流文件。
    static QString itemTempDir(const QString &roomId, const QString &clientId,
                               const QString &itemId);
    static void removeItemTempDir(const QString &roomId, const QString &clientId,
                                  const QString &itemId);
    static void removeRoomTempDir(const QString &roomId, const QString &clientId);

signals:
    void stateChanged();
    void probed();
    void ready(const QString &filePath);
    void failed(const QString &message);

private:
    // probe / inspect 共用的前半段:清状态 → 起进程跑 `-J --no-playlist`
    void startProbe(const QString &url, bool inspectOnly);
    // 起一个 yt-dlp:options 会在 URL **前面**,并自动补上 ──ffmpeg-location
    void startProcess(const QStringList &options, const QString &url);
    // 真的去试一下这个直链能不能拉(不带任何自定义头,和播放器处境一致)
    void verifyDirectUrl(const QString &url);
    // 掐掉还在飞的直链试拉,免得它的回调晚一步把已经作废的结果当成新的
    void abortVerify();
    void finishProbe();
    void handleProbeFinished();
    void handleDownloadLine(const QString &line);
    void handleDownloadFinished(int exitCode);
    void setStage(const QString &stage);
    void fail(const QString &message);

    const Toolchain *m_toolchain = nullptr;
    QProcess *m_process = nullptr;

    QString m_stage = QStringLiteral("idle");
    int m_progress = -1;
    QString m_detail;
    QString m_error;

    // 探测结果
    QString m_url;
    QString m_title;
    QString m_uploader;
    qint64 m_durationMs = 0;
    QString m_directUrl;
    bool m_isLive = false;
    // 画质档位列表(见 qualities 属性的说明)
    QVariantList m_qualities;
    // 当前这次解析的档位上限;probe/download 都按它挑格式
    int m_heightCap = 0;
    // 这次解析只是"看一看"(inspect):不挑直链、不试拉
    bool m_inspectOnly = false;

    // 下载用
    QString m_destDir;
    qint64 m_downloadedBytes = 0;
    qint64 m_totalBytes = 0;
    double m_speedBps = 0;
    int m_etaSec = -1;

    // yt-dlp 的输出尾巴,失败时原样给用户看(最后几行足够定位问题)
    QStringList m_outputTail;
    // cancelAndRemoveDir 记下来的目录:等进程退出后再删
    QString m_removeDirAfterExit;
    // 直链验证(异步)
    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_verifyReply = nullptr;
    QString m_verifyCandidate;
    QString m_verifyRejectReason;
    bool m_verifying = false;
    // 探测时把整份输出攒起来(JSON 可能被拆成多次 readyRead 到达)
    QByteArray m_outputBuffer;
};

#endif //SYNCINE_URLDOWNLOADER_H
