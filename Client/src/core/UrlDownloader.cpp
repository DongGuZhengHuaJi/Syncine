#include "UrlDownloader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

#include "core/CookieManager.h"
#include "core/Log.h"
#include "core/Toolchain.h"
#include "media/MediaPlatform.h"

namespace {

constexpr int kTailLines = 25; // 失败时留给用户看的输出行数

// "NA" 是 yt-dlp 在字段拿不到时的占位，不是数字
bool numberOrNa(const QString &text, double *value)
{
    if (text.isEmpty() || text == QLatin1String("NA"))
        return false;
    bool ok = false;
    const double parsed = text.toDouble(&ok);
    if (!ok)
        return false;
    *value = parsed;
    return true;
}

// 目录名只留安全字符：房间号/条目 id 都来自服务端，不能拿来拼路径
QString sanitize(const QString &text)
{
    QString result;
    for (const QChar ch : text) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_'))
            result.append(ch);
    }
    return result.isEmpty() ? QStringLiteral("unknown") : result;
}

QString formatSpeed(double bytesPerSecond)
{
    if (bytesPerSecond <= 0)
        return {};
    if (bytesPerSecond >= 1024 * 1024)
        return QStringLiteral("%1MB/s").arg(bytesPerSecond / (1024 * 1024), 0, 'f', 1);
    return QStringLiteral("%1KB/s").arg(bytesPerSecond / 1024, 0, 'f', 0);
}

QString formatEta(int seconds)
{
    if (seconds < 0)
        return {};
    return QStringLiteral("%1:%2").arg(seconds / 60)
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

// 从 yt-dlp 的输出里挑"原因"那一行。它出错时总是打一行 ERROR: 开头的话，
// 而周围混着别的东西(合并通道把 stderr 也并进来了，`-J` 失败时还会单独
// 输出一个 null)—— 整段原样显示会带一堆噪声，只挑 ERROR 那行最清楚。
// 挑不到（比如根本不是 yt-dlp 的报错）就退回整段原文。
QString errorFromOutput(const QStringList &output, const QString &fallback)
{
    for (int i = output.size() - 1; i >= 0; --i) {
        const QString line = output.at(i).trimmed();
        if (line.startsWith(QLatin1String("ERROR:")))
            return line;
    }
    return output.isEmpty() ? fallback : output.join(QLatin1Char('\n'));
}

// 手动钉的 format_id 在本端 formats 里不存在时，yt-dlp 会报这个。
// 这时退回"按档位重挑"再试一次（见 handleDownloadFinished）。
bool looksLikeFormatUnavailable(const QStringList &output)
{
    for (const QString &line : output) {
        if (line.contains(QLatin1String("Requested format is not available"))
            || line.contains(QLatin1String("format not available"))) {
            return true;
        }
    }
    return false;
}

} // namespace

UrlDownloader::UrlDownloader(const Toolchain *toolchain,
                             const CookieManager *cookies,
                             QObject *parent)
    : QObject(parent),
      m_toolchain(toolchain),
      m_cookies(cookies),
      m_network(new QNetworkAccessManager(this)) {

    // 单个定时器覆盖三种等待：探测/下载无输出、ffprobe 无输出。
    // 每次 readyRead 或进度行都会 restart，超时说明进程卡死。
    m_stallTimer.setSingleShot(true);
    connect(&m_stallTimer, &QTimer::timeout, this, &UrlDownloader::onStallTimeout);
}

UrlDownloader::~UrlDownloader() {
    stopStallTimer();
    for (QProcess *process : {m_process, m_hdrProbe}) {
        if (process == nullptr)
            continue;
        disconnect(process, nullptr, this, nullptr);
        process->kill();
        process->waitForFinished(1000);
        delete process;
    }
    m_process = nullptr;
    m_hdrProbe = nullptr;
}

// ============================
// 临时目录
// ============================

QString UrlDownloader::itemTempDir(const QString &roomId, const QString &clientId,
                                   const QString &itemId)
{
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation)
           + QStringLiteral("/syncine/") + sanitize(clientId)
           + QLatin1Char('/') + sanitize(roomId)
           + QLatin1Char('/') + sanitize(itemId);
}

void UrlDownloader::removeClientTempDir(const QString &clientId)
{
    QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)
         + QStringLiteral("/syncine/") + sanitize(clientId)).removeRecursively();
}

// ============================
// 解析
// ============================

void UrlDownloader::start(const QString &url, const QString &spec)
{
    if (m_toolchain == nullptr || !m_toolchain->ytDlpReady()) {
        fail(QStringLiteral("找不到 yt-dlp,无法解析链接"));
        return;
    }
    // URL 来自房间里的其他人（条目随房间下发），是不可信输入。启动进程前
    // 先挡一道，报错比让 yt-dlp 去解析畸形地址清楚
    if (!Toolchain::isSafeMediaUrl(url)) {
        fail(QStringLiteral("这个条目的地址不是有效的 http/https 链接，已跳过"));
        return;
    }

    // 新任务直接取代旧任务：cancel() 会同步 kill + waitForFinished + 置空，
    // 所以这里不会再有“旧进程还没结束，start 被静默吞掉”的窗口。
    // m_destDir 由调用方在 start 前设好，cancel 会清掉它，故先存一份。
    const QString destDir = m_destDir;
    if (isBusy()) {
        LOG_DEBUG("UrlDownloader") << "新任务取代未结束的旧任务";
        cancel();
    }
    m_destDir = destDir;

    startResolve(url, spec);
}

void UrlDownloader::startResolve(const QString &url, const QString &spec)
{
    m_url = url;
    m_spec = spec;
    m_media = MediaInfo();
    m_selection = MediaSelection();
    m_error.clear();
    m_progress = -1;
    m_detail.clear();
    m_hdrProbePath.clear();
    // 上一轮的精确流回退标记不能带到新任务里
    m_exactFallbackTried = false;
    setStage(QStringLiteral("probing"));

    startProcess({QStringLiteral("-J"), QStringLiteral("--no-playlist")}, url);
}

void UrlDownloader::handleResolveFinished(int exitCode)
{
    // 探测的 stdout 就是那一整份 JSON(合并通道后它可能和警告混在一起，
    // 所以从第一个 '{' 开始截)
    const QByteArray output = m_outputBuffer;
    const int start = output.indexOf('{');
    const QJsonDocument document = start >= 0
        ? QJsonDocument::fromJson(output.mid(start))
        : QJsonDocument();
    if (exitCode != 0 || !document.isObject()) {
        fail(errorFromOutput(m_outputTail, QStringLiteral("yt-dlp 没能解析这个链接")));
        return;
    }

    m_media = MediaFormats::parseMediaInfo(document.object());
    m_selection = MediaFormats::resolve(m_spec, m_media, m_allowHdr);

    if (!m_selection.valid) {
        // resolve() 会给出具体原因（例如“关闭 HDR 后该档位没有 SDR 流”）
        reportFailure(!m_selection.error.isEmpty()
                          ? m_selection.error
                          : (m_media.live
                                 ? QStringLiteral("这个直播源没有可直接播放的流"
                                                  "，请检查输入内容或Cookie状态")
                                 : QStringLiteral("这个视频源没有可供下载的流"
                                                  "，请检查输入内容或Cookie状态")));
        return;
    }

    LOG_INFO("UrlDownloader") << "解析完成:" << m_media.title
                              << QStringLiteral("spec[%1]").arg(m_spec)
                              << "→" << m_selection.label
                              << (m_selection.direct ? "(直链)" : "(下载合并)");

    // 直链与下载合并都要先让人知道"探测结果已就绪"：下载路径要等
    // ready() 才发 resolved()，弹幕等到那会儿就太晚了。
    emit probed();

    if (m_selection.direct) {
        // 合一格式（或直播的 m3u8）:试一下能不能直接拉 —— 能就交给播放器
        m_verifyCandidate = m_selection.url;
        verifyDirectUrl(m_verifyCandidate);
        return;
    }

    beginDownload(exitCode);
}

void UrlDownloader::verifyDirectUrl(const QString &url)
{
    QNetworkRequest request{QUrl(url)};
    // 测试一个字节，验证能否响应
    // 不携带消息头（Referer/Cookie） —— 后续播放器无法携带
    request.setRawHeader("Range", "bytes=0-0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(8000);

    QNetworkReply *reply = m_network->get(request);
    m_verifyReply = reply;
    auto *gotData = new bool(false);

    connect(reply, &QNetworkReply::readyRead, this, [reply, gotData]() {
        // 不做重复处理
        if (*gotData)
            return;
        // 检测 HTTP 状态码范围
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status < 200 || status >= 300)
            return;
        *gotData = true;
        reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, gotData]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // HTTP 200/206 认为成功
        const bool ok = *gotData || status == 200 || status == 206;
        delete gotData;
        reply->deleteLater();
        m_verifyReply = nullptr;

        if (ok) {
            LOG_INFO("UrlDownloader") << "直链可直接拉取(HTTP" << status << ")";
            m_selection.url = m_verifyCandidate;
            setStage(QStringLiteral("resolved"));
            emit resolved();
            return;
        }

        LOG_WARN("UrlDownloader") << "直链试拉失败: HTTP" << status << reply->errorString();
        if (m_media.live) {
            fail(QStringLiteral("这个直播源拉不动(HTTP %1,通常要 Referer/Cookie,"
                                "播放器加不了请求头)")
                     .arg(status > 0 ? QString::number(status) : QStringLiteral("连接失败")));
            return;
        }
        m_selection.direct = false;
        beginDownload(0);
    });
}

void UrlDownloader::beginDownload(int /*exitCode*/)
{
    if (!m_selection.valid) {
        fail(!m_selection.error.isEmpty() ? m_selection.error
                                          : QStringLiteral("没有可下载的流"));
        return;
    }
    if (m_destDir.isEmpty()) {
        // 没指定目录就放在系统临时目录里，每次新建一个 syncine/download 子目录
        m_destDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                    + QStringLiteral("/syncine/download");
    }
    if (m_toolchain == nullptr || !m_toolchain->ffmpegReady()) {
        fail(QStringLiteral("找不到 ffmpeg,画面/声音分离的源无法下载合并"));
        return;
    }

    m_totalBytes = 0;
    m_currentFileBytes = 0;
    m_finishedBytes = 0;
    m_speedBps = 0;
    m_etaSec = -1;
    m_progress = -1;
    m_error.clear();
    m_outputTail.clear();

    // 分离流要下两条，按解析到的确切大小估算整体进度。拿不到大小时退回
    // 单条流的百分比（见 handleDownloadLine），绝不会算出负数。
    m_expectedTotalBytes = 0;
    if (m_selection.video.kind == MediaFormat::Combined) {
        m_expectedTotalBytes = m_selection.video.bestSize();
    } else {
        const qint64 videoBytes = m_selection.video.bestSize();
        const qint64 audioBytes = m_selection.audio.bestSize();
        if (videoBytes > 0 && audioBytes > 0)
            m_expectedTotalBytes = videoBytes + audioBytes;
    }
    QDir().mkpath(m_destDir);

    // 清理旧残留，避免 glob "media.*" 拿到上次的文件
    const QDir destDirInfo(m_destDir);
    for (const QString &name : destDirInfo.entryList({QStringLiteral("media.*")}, QDir::Files))
        QFile::remove(destDirInfo.absoluteFilePath(name));
    setStage(QStringLiteral("downloading"));

    // 输出名固定成 media.<ext>:扩展名事先不知道（要看源格式，合并后是 mkv）,
    // 结束时在目录里 glob "media.*" 就能拿到。
    m_outputTemplate = m_destDir + QStringLiteral("/media.%(ext)s");
    // `--progress-template` 中冒号前是**类型**（download/postprocess/...），
    // 不是字面输出。要得到一个可识别的标记，标记必须写在模板本身里 ——
    // 曾把 "download:" 当成前缀解析，结果所有进度行都被丢弃，界面永远停在
    // 「下载中…」。
    const QString progressTemplate = QStringLiteral(
        "download:SYNCINE %(progress.downloaded_bytes)s %(progress.total_bytes)s "
        "%(progress.total_bytes_estimate)s %(progress.speed)s %(progress.eta)s");

    QStringList options{QStringLiteral("--newline"),
                        QStringLiteral("--progress-template"), progressTemplate,
                        QStringLiteral("--no-playlist"),
                        QStringLiteral("-o"), m_outputTemplate};

    if (m_selection.video.kind == MediaFormat::Combined && !m_selection.formatExpression.isEmpty()) {
        // 合一格式（用户手选，或按档位选中的精确合一流）：原样照下
        options << QStringLiteral("-f") << m_selection.formatExpression;
    } else if (!m_selection.formatExpression.isEmpty()) {
        // 精确流对（用户手选，或按档位选中的画面+声音）：
        // 合并格式按容器名定（mkv 能装几乎所有编码组合，也是最稳的）。
        options << QStringLiteral("-f") << m_selection.formatExpression
                << QStringLiteral("--merge-output-format") << QStringLiteral("mkv");
    } else if (!m_selection.sortArgs.isEmpty()) {
        // 对方没有精确流时的回退：用 -S 排序。
        // 注意 -S 只是偏好，不能保证不选 HDR；关闭 HDR 时下载后还会走 ffprobe 复核。
        options << QStringLiteral("-f") << QStringLiteral("bv*+ba/b")
                << QStringLiteral("-S") << m_selection.sortArgs.first()
                << QStringLiteral("--merge-output-format") << QStringLiteral("mkv");
    } else {
        options << QStringLiteral("-f") << QStringLiteral("bv*+ba/b")
                << QStringLiteral("--merge-output-format") << QStringLiteral("mkv");
    }

    LOG_INFO("UrlDownloader") << "开始下载:" << m_selection.label
                              << "→" << m_destDir;
    startProcess(options, m_url);
}

// ============================
// 下载
// ============================

void UrlDownloader::handleDownloadLine(const QString &line)
{
    // yt-dlp 在合并时会打 "[Merger] Merging formats into ..." ——
    // 这一段没有任何进度输出，不告诉用户的话界面会卡在 100% 不动。
    // 合并是 ffmpeg 的 remux，通常很快；大文件/慢磁盘给 3 分钟，
    // 避免 60 秒的下载看门狗把正常的合并误杀。
    if (line.contains(QLatin1String("[Merger]"))) {
        m_progress = 100;
        setStage(QStringLiteral("merging"));
        startStallTimer(180000);
        emit stateChanged();
        return;
    }

    // 标记由上面的 progressTemplate 输出，形如 "SYNCINE 1024 20399323 NA ..."
    if (!line.startsWith(QLatin1String("SYNCINE ")))
        return;

    const QStringList fields = line.mid(8).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (fields.size() < 5)
        return;

    double value = 0;
    if (numberOrNa(fields.at(0), &value)) {
        const qint64 downloaded = static_cast<qint64>(value);
        // 换到下一条流：后一条的计数从 0 开始，把上一条已下的字节并入基数
        if (downloaded < m_currentFileBytes && m_currentFileBytes > 0)
            m_finishedBytes += m_currentFileBytes;
        m_currentFileBytes = downloaded;
    }

    if (numberOrNa(fields.at(1), &value)) {
        m_totalBytes = static_cast<qint64>(value);
    } else if (numberOrNa(fields.at(2), &value)) {
        // 总大小经常没有（DASH/HLS 事先不知道）,回退到估算值，否则算不出百分比
        m_totalBytes = static_cast<qint64>(value);
    }

    if (numberOrNa(fields.at(3), &value))
        m_speedBps = value;
    if (numberOrNa(fields.at(4), &value))
        m_etaSec = static_cast<int>(value);

    if (m_expectedTotalBytes > 0) {
        const qint64 overall = m_finishedBytes + m_currentFileBytes;
        const qint64 percent = qMin<qint64>(100, overall * 100 / m_expectedTotalBytes);
        m_progress = static_cast<int>(qMax<qint64>(0, percent));
    } else if (m_totalBytes > 0) {
        const qint64 percent = qMin<qint64>(100, m_currentFileBytes * 100 / m_totalBytes);
        m_progress = static_cast<int>(qMax<qint64>(0, percent));
    } else {
        m_progress = -1; // 总大小未知(DASH/HLS 常见)—— 界面显示不确定进度
    }

    QStringList parts;
    const QString speed = formatSpeed(m_speedBps);
    if (!speed.isEmpty())
        parts << speed;
    const QString eta = formatEta(m_etaSec);
    if (!eta.isEmpty())
        parts << QStringLiteral("剩 %1").arg(eta);
    m_detail = parts.join(QStringLiteral(" · "));

    emit stateChanged();
}

void UrlDownloader::handleDownloadFinished(int exitCode)
{
    if (exitCode != 0) {
        // 精确的 format_id 在本端不存在（登录态/会员等级和添加者不同，或关闭
        // HDR 时钉死的 SDR 流对方没有）时，退回"按档位挑"再试一次 ——
        // 别让"这条下不到"变成"这条播不了"。回退仍遵守本地 HDR 开关。
        if (!m_selection.formatExpression.isEmpty()
            && looksLikeFormatUnavailable(m_outputTail)
            && !m_exactFallbackTried) {
            m_exactFallbackTried = true;
            const int height = m_selection.video.height;
            // 回退也只能在“条目 spec 允许 HDR 且本地设置允许”时选 HDR；
            // 不能用 m_allowHdr 单独决定，否则会把条目的 SDR 意图放宽成 HDR。
            const bool fallbackHdr = StreamSpec::hdrAllowed(m_spec) && m_allowHdr;
            LOG_WARN("UrlDownloader") << "精确流在本端不存在,退回按档位解析:"
                                      << m_selection.formatExpression;
            m_selection = MediaFormats::resolve(
                StreamSpec::withHdr(height > 0 ? QString::number(height) : QString(),
                                    fallbackHdr),
                m_media, m_allowHdr);
            if (!m_selection.valid) {
                fail(!m_selection.error.isEmpty()
                         ? m_selection.error
                         : QStringLiteral("退回按档位解析后仍没有可用的流"));
                return;
            }
            // 精确 id 既然下不到，再钉一次同样的 id 没意义；这次改用纯 -S 排序。
            // 关闭 HDR 时排序键含 dynamic_range:sdr，下载后还有 ffprobe 复核。
            m_selection.formatExpression.clear();
            beginDownload(0);
            return;
        }

        fail(errorFromOutput(m_outputTail,
                             QStringLiteral("yt-dlp 退出码 %1").arg(exitCode)));
        return;
    }

    // 寻找最终产物
    const QStringList finalExts{QStringLiteral("mkv"), QStringLiteral("mp4"),
                                QStringLiteral("webm"), QStringLiteral("mov"),
                                QStringLiteral("flv"), QStringLiteral("avi")};
    const QDir dir(m_destDir);
    const QStringList entries = dir.entryList({QStringLiteral("media.*")}, QDir::Files);
    for (const QString &name : entries) {
        // "media." 之后还有一层点 = 中间流文件或未完成文件，都不是产物
        if (name.mid(6).contains(QLatin1Char('.')))
            continue;
        if (!finalExts.contains(QFileInfo(name).suffix().toLower()))
            continue;

        const QString path = dir.absoluteFilePath(name);
        const qint64 bytes = QFileInfo(path).size();
        LOG_INFO("UrlDownloader") << "下载完成:" << path
                                  << QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 1);
        if (m_allowHdr) {
            finishReady(path);
        } else {
            // ffprobe复核
            startHdrProbe(path);
        }
        return;
    }

    fail(QStringLiteral("下载结束但没找到产物文件"));
}

// ============================
// 进程
// ============================

void UrlDownloader::abortVerify()
{
    if (m_verifyReply == nullptr)
        return;

    // 先断开再 abort:abort 也会发 finished,不断开的话那个回调会把它当成
    // "试拉结束"、拿着已经作废的候选往下走
    disconnect(m_verifyReply, nullptr, this, nullptr);
    m_verifyReply->abort();
    m_verifyReply->deleteLater();
    m_verifyReply = nullptr;
    m_verifyCandidate.clear();
}

void UrlDownloader::startProcess(const QStringList &options, const QString &url)
{
    abortVerify(); // 新任务开始,旧任务的试拉结果一律作废

    m_outputTail.clear();
    m_outputBuffer.clear();

    auto *process = new QProcess(this);
    m_process = process;
    // 合并 stdout/stderr:进度和错误信息在一个流里，顺序也真实
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setProgram(m_toolchain->ytDlpPath());

    // 拼接参数：用户选项 + ffmpeg-location + cookies + url
    // url 在末尾，其余无限制
    QStringList args = options;
    args << m_toolchain->ffmpegLocationArgs();
    // 只有平台策略允许时才带 cookie：B 站始终带，Generic 默认不带，
    // 避免把 B 站登录态发给任意站点。
    const MediaPlatform *platform = mediaPlatformRegistry().detect(QUrl(m_url));
    if (m_cookies != nullptr && platformUsesCookies(platform, m_genericCookies))
        args << m_cookies->cookieArgs();
    Toolchain::appendUrlArg(args, url); // 必须走它，理由见 Toolchain.h
    process->setArguments(args);

    LOG_DEBUG("UrlDownloader") << "yt-dlp" << args.join(QLatin1Char(' '));

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        if (m_process != process)
            return;
        const QByteArray raw = process->readAllStandardOutput();
        // 探测阶段要攒整份 JSON；下载阶段只留尾巴，避免长下载无限涨内存
        if (m_stage == QLatin1String("probing"))
            m_outputBuffer.append(raw);
        // 有输出就说明进程还活着，喂一次看门狗
        m_stallTimer.start();

        const QString chunk = QString::fromUtf8(raw);
        for (const QString &line : chunk.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            m_outputTail.append(line);
            while (m_outputTail.size() > kTailLines)
                m_outputTail.removeFirst();

            if (m_stage == QLatin1String("downloading")
                || m_stage == QLatin1String("merging")) {
                handleDownloadLine(line);
            }
        }
    });

    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || m_process != process)
                    return;
                // FailedToStart 不会再有 finished()：必须在这里同步清理，
                // 否则 m_process 永远非空，后续 start() 全部被吞。
                stopStallTimer();
                const QString reason = process->errorString();
                disconnect(process, nullptr, this, nullptr);
                if (m_process == process)
                    m_process = nullptr;
                process->deleteLater();
                fail(QStringLiteral("yt-dlp 启动失败:%1").arg(reason));
            });

    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus status) {
                if (m_process != process) {
                    process->deleteLater();
                    return;
                }
                const QString stage = m_stage;
                stopStallTimer();

                m_process = nullptr;
                process->deleteLater();

                // cancel() 会把阶段设回 idle —— 这种"异常结束"是用户自己取消的，
                // 不能报成失败
                if (stage == QLatin1String("idle")) {
                    LOG_INFO("UrlDownloader") << "yt-dlp 被用户取消";
                    return;
                }

                if (stage != QLatin1String("probing")
                    && stage != QLatin1String("downloading")
                    && stage != QLatin1String("merging")) {
                    LOG_DEBUG("UrlDownloader") << "yt-dlp 在阶段" << stage << "结束,忽略";
                    return;
                }

                if (status != QProcess::NormalExit) {
                    fail(QStringLiteral("yt-dlp 异常结束"));
                    return;
                }

                if (stage == QLatin1String("probing"))
                    handleResolveFinished(exitCode);
                else
                    handleDownloadFinished(exitCode);
            });

    // 探测 30 秒、下载 60 秒没有输出就判卡死；进入合并阶段后放宽到 3 分钟
    // （见 handleDownloadLine 的 [Merger] 分支）。
    startStallTimer(m_stage == QLatin1String("probing") ? 30000 : 60000);
    process->start();
}

void UrlDownloader::reportFailure(const QString &message)
{
    fail(message);
}

bool UrlDownloader::isBusy() const
{
    return m_process != nullptr || m_hdrProbe != nullptr || m_verifyReply != nullptr;
}

void UrlDownloader::cleanupProcess(QProcess *&process)
{
    QProcess *old = process;
    process = nullptr;
    if (old == nullptr)
        return;

    // 先断开再 kill：kill 后 QProcess 会发 finished，若不拦住，回调会拿
    // 一个已经被放弃的任务去 handleResolve/DownloadFinished。
    disconnect(old, nullptr, this, nullptr);
    if (old->state() != QProcess::NotRunning) {
        old->kill();
        old->waitForFinished(1000);
    }
    old->deleteLater();
}

void UrlDownloader::startStallTimer(int timeoutMs)
{
    m_stallTimer.start(timeoutMs);
}

void UrlDownloader::stopStallTimer()
{
    m_stallTimer.stop();
}

void UrlDownloader::onStallTimeout()
{
    if (m_hdrProbe != nullptr) {
        LOG_WARN("UrlDownloader") << "ffprobe 超时,无法确认动态范围";
        const QString path = m_hdrProbePath;
        m_hdrProbePath.clear();
        cleanupProcess(m_hdrProbe);
        if (!path.isEmpty())
            QFile::remove(path);
        fail(QStringLiteral("ffprobe 超时，无法确认下载文件的动态范围"));
        return;
    }

    if (m_process == nullptr)
        return;

    LOG_WARN("UrlDownloader") << "yt-dlp 长时间无输出,主动终止";
    cleanupProcess(m_process);
    fail(QStringLiteral("yt-dlp 无响应（长时间没有输出），请检查网络后重试"));
}

void UrlDownloader::startHdrProbe(const QString &path)
{
    if (m_toolchain == nullptr || m_toolchain->ffprobePath().isEmpty()) {
        QFile::remove(path);
        fail(QStringLiteral("找不到 ffprobe，无法确认下载文件的动态范围"));
        return;
    }

    m_hdrProbePath = path;
    auto *probe = new QProcess(this);
    m_hdrProbe = probe;
    probe->setProgram(m_toolchain->ffprobePath());
    probe->setArguments({
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-select_streams"), QStringLiteral("v:0"),
        QStringLiteral("-show_entries"), QStringLiteral("stream=color_transfer"),
        QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1:nokey=1"),
        path,
    });

    connect(probe, &QProcess::finished, this,
            [this, probe](int exitCode, QProcess::ExitStatus) {
                if (m_hdrProbe != probe)
                    return;
                handleHdrProbeFinished(exitCode);
            });
    connect(probe, &QProcess::errorOccurred, this,
            [this, probe](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || m_hdrProbe != probe)
                    return;
                const QString path = m_hdrProbePath;
                m_hdrProbePath.clear();
                cleanupProcess(m_hdrProbe);
                if (!path.isEmpty())
                    QFile::remove(path);
                fail(QStringLiteral("ffprobe 启动失败，无法确认下载文件的动态范围"));
            });

    LOG_DEBUG("UrlDownloader") << "复核动态范围:" << path;
    setStage(QStringLiteral("checking"));
    m_stallTimer.start(10000); // ffprobe 读本地文件，10 秒足够
    probe->start();
}

void UrlDownloader::handleHdrProbeFinished(int exitCode)
{
    QProcess *probe = m_hdrProbe;
    m_hdrProbe = nullptr;
    stopStallTimer();

    QString output;
    if (probe != nullptr) {
        output = QString::fromUtf8(probe->readAllStandardOutput()).trimmed();
        probe->deleteLater();
    }

    const QString path = m_hdrProbePath;
    m_hdrProbePath.clear();
    const QString transfer = output.toLower();
    const bool hdr = transfer.contains(QLatin1String("smpte2084"))
                     || transfer.contains(QLatin1String("arib-std-b67"));

    if (exitCode != 0 || output.isEmpty()) {
        QFile::remove(path);
        fail(QStringLiteral("无法确认下载文件的动态范围（ffprobe 没有返回 color_transfer）"));
        return;
    }
    if (hdr) {
        QFile::remove(path);
        LOG_WARN("UrlDownloader") << "下载到 HDR 流,按设置拒绝:" << path
                                  << "color_transfer=" << output;
        fail(QStringLiteral("下载到的是 HDR 流，已按设置拒绝；"
                            "请在设置中允许 HDR，或选择 SDR 档位"));
        return;
    }

    finishReady(path);
}

void UrlDownloader::finishReady(const QString &path)
{
    m_progress = 100;
    setStage(QStringLiteral("ready"));
    emit ready(path);
}

void UrlDownloader::cancel()
{
    stopStallTimer();

    if (m_process != nullptr) {
        LOG_INFO("UrlDownloader") << "已取消";
        // 同步杀掉并置空：调用方（retry）紧接着就会 start()，
        // 不能把“旧进程还没结束”留给下一次 start 去撞。
        cleanupProcess(m_process);
    }
    if (m_hdrProbe != nullptr) {
        cleanupProcess(m_hdrProbe);
        // 还没通过复核的文件不留下，否则下次可能被当成缓存
        if (!m_hdrProbePath.isEmpty())
            QFile::remove(m_hdrProbePath);
    }
    m_hdrProbePath.clear();
    abortVerify();

    m_progress = -1;
    m_detail.clear();
    m_error.clear();
    m_url.clear();
    m_spec.clear();
    m_media = MediaInfo();
    m_selection = MediaSelection();
    m_destDir.clear();
    m_outputTemplate.clear();
    m_outputTail.clear();
    m_outputBuffer.clear();
    m_currentFileBytes = 0;
    m_finishedBytes = 0;
    m_expectedTotalBytes = 0;
    m_exactFallbackTried = false;
    const bool stageChanged = m_stage != QLatin1String("idle");
    setStage(QStringLiteral("idle"));
    // 本来就是 idle 时 setStage 会去重（不发信号）,可上面那一串清空同样
    // 需要让绑定重算 —— 补一次，别让界面留着上次的数据
    if (!stageChanged)
        emit stateChanged();
}

// ============================
// 状态
// ============================

void UrlDownloader::setStage(const QString &stage)
{
    if (m_stage == stage)
        return;
    m_stage = stage;
    emit stateChanged();
}

void UrlDownloader::fail(const QString &message)
{
    stopStallTimer();
    m_error = message;
    m_progress = -1;
    m_detail.clear();
    LOG_WARN("UrlDownloader") << "失败:" << message;
    setStage(QStringLiteral("failed"));
    emit failed(message);
}
