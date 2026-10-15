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
}

UrlDownloader::~UrlDownloader() {
    if (m_process != nullptr) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
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

void UrlDownloader::removeRoomTempDir(const QString &roomId, const QString &clientId)
{
    QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)
         + QStringLiteral("/syncine/") + sanitize(clientId)
         + QLatin1Char('/') + sanitize(roomId)).removeRecursively();
}

// ============================
// 解析
// ============================

void UrlDownloader::start(const QString &url, const QString &spec)
{
    if (m_process != nullptr) {
        LOG_DEBUG("UrlDownloader") << "上一次任务还没结束,忽略这次解析";
        return;
    }
    if (m_toolchain == nullptr || !m_toolchain->ytDlpReady()) {
        fail(QStringLiteral("找不到 yt-dlp,无法解析链接"));
        return;
    }

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
    m_selection = MediaFormats::resolve(m_spec, m_media);

    if (!m_selection.valid) {
        // 直播走到这里 = 没有能直接拉的流（通常是要登录/要特殊请求头）;
        // 普通视频走到这里 = 连一条画面都挑不出来（空 formats 之类）
        reportFailure(m_media.live
                          ? QStringLiteral("这个直播源没有可直接播放的流"
                                           "，请检查输入内容或Cookie状态")
                          : QStringLiteral("这个视频源没有可供下载的流"
                                            "，请检查输入内容或Cookie状态"));
        return;
    }

    LOG_INFO("UrlDownloader") << "解析完成:" << m_media.title
                              << QStringLiteral("spec[%1]").arg(m_spec)
                              << "→" << m_selection.label
                              << (m_selection.direct ? "(直链)" : "(下载合并)");

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
    if (m_destDir.isEmpty()) {
        // 没指定目录就放在系统临时目录里，每次新建一个 syncine/download 子目录
        m_destDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                    + QStringLiteral("/syncine/download");
    }
    if (m_toolchain == nullptr || !m_toolchain->ffmpegReady()) {
        fail(QStringLiteral("找不到 ffmpeg,画面/声音分离的源无法下载合并"));
        return;
    }

    m_downloadedBytes = 0;
    m_totalBytes = 0;
    m_speedBps = 0;
    m_etaSec = -1;
    m_progress = -1;
    m_error.clear();
    m_outputTail.clear();
    QDir().mkpath(m_destDir);

    // 清理旧残留，避免 glob "media.*" 拿到上次的文件
    const QDir destDirInfo(m_destDir);
    for (const QString &name : destDirInfo.entryList({QStringLiteral("media.*")}, QDir::Files))
        QFile::remove(destDirInfo.absoluteFilePath(name));
    setStage(QStringLiteral("downloading"));

    // 输出名固定成 media.<ext>:扩展名事先不知道（要看源格式，合并后是 mkv）,
    // 结束时在目录里 glob "media.*" 就能拿到。
    m_outputTemplate = m_destDir + QStringLiteral("/media.%(ext)s");
    const QString progressTemplate = QStringLiteral(
        "download:%(progress.downloaded_bytes)s %(progress.total_bytes)s "
        "%(progress.total_bytes_estimate)s %(progress.speed)s %(progress.eta)s");

    QStringList options{QStringLiteral("--newline"),
                        QStringLiteral("--progress-template"), progressTemplate,
                        QStringLiteral("--no-playlist"),
                        QStringLiteral("-o"), m_outputTemplate};

    if (m_selection.video.kind == MediaFormat::Combined && !m_selection.formatExpression.isEmpty()) {
        // 用户手选的是一条合一格式：原样照下，不转换不合并
        options << QStringLiteral("-f") << m_selection.formatExpression;
    } else if (!m_selection.formatExpression.isEmpty()) {
        // 用户手选的一对具体流：精确指定。
        // 合并格式按容器名定（mkv 能装几乎所有编码组合，也是最稳的）:
        // 源本身就是单文件 mp4 时不重封装。
        options << QStringLiteral("-f") << m_selection.formatExpression
                << QStringLiteral("--merge-output-format") << QStringLiteral("mkv");
    } else if (!m_selection.sortArgs.isEmpty()) {
        // 档位：不写 -f,让 yt-dlp 按 -S 的优先级自己挑(见 FormatModel.h 顶部
        // 为什么不写 `-f "bv*[height<=1080]"` —— 那是筛选，不是排序，实测会降档)
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
    // 这一段没有任何进度输出，不告诉用户的话界面会卡在 100% 不动
    if (line.contains(QLatin1String("[Merger]"))) {
        setStage(QStringLiteral("merging"));
        emit stateChanged();
        return;
    }

    if (!line.startsWith(QLatin1String("download:")))
        return;

    const QStringList fields = line.mid(9).split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (fields.size() < 5)
        return;

    double value = 0;
    if (numberOrNa(fields.at(0), &value))
        m_downloadedBytes = static_cast<qint64>(value);

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

    if (m_totalBytes > 0) {
        const qint64 percent = qMin<qint64>(100, m_downloadedBytes * 100 / m_totalBytes);
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
        // 手选的 format_id 在本端不存在（登录态/会员等级和添加者不同）时，
        // 退回"按档位挑"再试一次 —— 别让"这条下不到"变成"这条播不了"。
        // **注意：这里只对"手选的精确流"这么做**;档位下载本身不会遇到这个错。
        if (!m_selection.formatExpression.isEmpty()
            && looksLikeFormatUnavailable(m_outputTail)
            && !m_manualFallbackTried) {
            m_manualFallbackTried = true;
            const int height = m_selection.video.height;
            LOG_WARN("UrlDownloader") << "手选的流在本端不存在,退回按档位解析:"
                                      << m_selection.formatExpression;
            m_selection = MediaFormats::resolve(
                StreamSpec::withHdr(height > 0 ? QString::number(height) : QString(),
                                    /*allowHdr=*/true),
                m_media);
            beginDownload(0);
            return;
        }

        fail(errorFromOutput(m_outputTail,
                             QStringLiteral("yt-dlp 退出码 %1").arg(exitCode)));
        return;
    }

    // 找产物：**只认最终的那个 media.<ext>**(合并完是 mkv,单文件格式保持原扩展名)。
    //
    // 中间流文件形如 media.<format_id>.<ext>，例如 media.f30280.m4a 为纯音频——
    // 上一轮失败残留的此类文件若被当作产物加载，症状为「只有声音」（实测）。
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
        m_progress = 100;
        setStage(QStringLiteral("ready"));
        emit ready(path);
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

    m_process = new QProcess(this);
    // 合并 stdout/stderr:进度和错误信息在一个流里，顺序也真实
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setProgram(m_toolchain->ytDlpPath());

    // 拼接参数：用户选项 + ffmpeg-location + cookies + url
    // url 在末尾，其余无限制
    QStringList args = options;
    args << m_toolchain->ffmpegLocationArgs();
    if (m_cookies != nullptr)
        args << m_cookies->cookieArgs();
    args << url;
    m_process->setArguments(args);

    LOG_DEBUG("UrlDownloader") << "yt-dlp" << args.join(QLatin1Char(' '));

    connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        const QByteArray raw = m_process->readAllStandardOutput();
        // 缓存输出
        m_outputBuffer.append(raw);

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

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(QStringLiteral("yt-dlp 启动失败:%1").arg(m_process->errorString()));
    });

    connect(m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus status) {
                const QString stage = m_stage;

                QProcess *finished = m_process;
                m_process = nullptr;
                if (finished != nullptr)
                    finished->deleteLater();

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

    m_process->start();
}

void UrlDownloader::reportFailure(const QString &message)
{
    fail(message);
}

void UrlDownloader::cancel()
{
    if (m_process != nullptr) {
        LOG_INFO("UrlDownloader") << "已取消";
        m_process->kill();
    }
    abortVerify();

    // 复位：界面（加条目弹窗）关掉/重开时要回到干净状态。
    // **没有进程在跑时也要清** —— 上一次解析成功留下的标题、档位列表
    // 不清掉的话，下次打开弹窗会直接看到上一次的结果。
    //
    // 这里**只发 stateChanged**:setStage 对相同取值去重，而 reset 之后
    // 各属性都变了，得让绑定有机会重算。千万别在这里 emit resolved() ——
    // PlaybackSync::retryCurrentItem 是先 cancel 再重新准备，
    // 一个多余的 resolved 会让它拿着刚被清空的直链直接去下载。
    m_progress = -1;
    m_detail.clear();
    m_error.clear();
    m_url.clear();
    m_spec.clear();
    m_media = MediaInfo();
    m_selection = MediaSelection();
    m_destDir.clear();
    m_outputTemplate.clear();
    m_manualFallbackTried = false;
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
    m_error = message;
    m_progress = -1;
    m_detail.clear();
    LOG_WARN("UrlDownloader") << "失败:" << message;
    setStage(QStringLiteral("failed"));
    emit failed(message);
}
