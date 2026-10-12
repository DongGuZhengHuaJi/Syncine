#include "UrlDownloader.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QVariantMap>

#include "core/CookieManager.h"
#include "core/Log.h"
#include "core/Toolchain.h"

namespace {

constexpr int kTailLines = 25; // 失败时留给用户看的输出行数

// "NA" 是 yt-dlp 在字段拿不到时的占位,不是数字
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

// 目录名只留安全字符:房间号/条目 id 都来自服务端,不能拿来拼路径
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

// 这批格式里有没有 HLS(m3u8)的
bool isHls(const QJsonArray &formats)
{
    for (const QJsonValue &value : formats) {
        if (value.toObject().value(QStringLiteral("protocol")).toString()
                .contains(QLatin1String("m3u8"))) {
            return true;
        }
    }
    return false;
}

QVariantMap qualityEntry(const QString &label, int height)
{
    QVariantMap map;
    map[QStringLiteral("label")] = label;
    map[QStringLiteral("height")] = height;
    return map;
}

// 把 formats 归纳成"人话的"画质档位:按画面高度去重、从高到低。
//
// 为什么要归纳而不是把几十条 formats 直接列出来:同一个高度通常有好几条
// (B 站 1080P 就有 h264 / hevc / av1 三种),对用户来说它们是同一档;
// 到底用哪条编码是 yt-dlp 排序的事,不该让用户选。
//
// 首项固定是 { 最高画质, height=0 } —— 它和"最新版 yt-dlp 自己挑的"等价,
// 也就是这个功能上线之前的行为,所以默认选它 = 什么都没变。
QVariantList qualityOptionsFromFormats(const QJsonArray &formats, bool isLive)
{
    QVariantList options;

    // 直播的画质是清单自己协商的(EXT-X-MEDIA / 播放器自适应),
    // 没有"档位"这回事 —— 只给一项,界面会把它整块藏起来。
    if (isLive) {
        options.append(qualityEntry(QStringLiteral("最高画质"), 0));
        return options;
    }

    QHash<int, double> bestFps; // 高度 → 该高度下的最高帧率
    QList<int> heights;
    for (const QJsonValue &value : formats) {
        const QJsonObject format = value.toObject();
        // 纯音频(vcodec 为 none)和没有高度的项(少数直链文件)不构成画质档位
        if (format.value(QStringLiteral("vcodec")).toString() == QLatin1String("none"))
            continue;
        const int height = format.value(QStringLiteral("height")).toInt(0);
        if (height <= 0)
            continue;

        if (!heights.contains(height))
            heights.append(height);

        const double fps = format.value(QStringLiteral("fps")).toDouble(0);
        if (fps > bestFps.value(height, 0.0))
            bestFps.insert(height, fps);
    }

    std::sort(heights.begin(), heights.end(), std::greater<int>());

    options.append(qualityEntry(QStringLiteral("最高画质"), 0));
    for (const int height : heights) {
        QString label = QStringLiteral("%1P").arg(height);
        // 59.94 这种也按 60 算,阈值取 50 比 60 稳
        if (bestFps.value(height, 0.0) >= 50.0)
            label += QStringLiteral("60");
        options.append(qualityEntry(label, height));
    }
    return options;
}

QString pickDirectFormat(const QJsonArray &formats, bool isLive, int heightCap,
                         QString *rejectReason)
{
    // ---- 直播:HLS 的 master 清单 ----
    //
    // HLS 有个别的协议没有的能力:master 清单里能用 EXT-X-MEDIA 把音视频
    // 分成两条列出来,**播放器自己会把它们合起来播**。所以对 HLS 不挑具体格式,
    // 直接把 master 清单交给播放器就行。
    //
    // 这不是优化,是**直播唯一可行的形态**:直播源基本都是音视频分离的,
    // 而直播没有"下完"这一刻,下载合并对它不成立。
    // (DASH/mpd 没有这个能力 —— ffmpeg 只有 dash 封装器,没有解封装器,
    //  所以那种源只能走下载合并。)
    //
    // **只有直播才走这条路**:拿 HLS 换来的代价是"时长未知、不能拖进度"
    // (实测 Apple 的 HLS 样片:加载成功但 duration=0、seekable=false)。
    // 普通视频那样做就丢掉了拖动和位置同步,所以 VOD 一律走下面那两条。
    //
    // 挑法(按优先级):
    //   1. h264 的 m3u8 —— 兼容性最好
    //   2. 任意 m3u8
    // 地址优先用 `manifest_url`(master 清单,能带 EXT-X-MEDIA);
    // **它为空时用格式自己的 url** —— 实测 B 站直播就是这种:manifest_url 为空,
    // 但那条 url 指的 m3u8 里 h264 + aac 都有,而且不带 Referer 也能拉,
    // 这才是能用的那一条(它同时列出的 HEVC FLV 反而要 Referer)。
    QString hlsAny;
    QString hlsAvc;
    for (const QJsonValue &value : isLive ? formats : QJsonArray()) {
        const QJsonObject format = value.toObject();
        if (!format.value(QStringLiteral("protocol")).toString()
                 .contains(QLatin1String("m3u8"))) {
            continue;
        }

        QString candidate = format.value(QStringLiteral("manifest_url")).toString();
        if (candidate.isEmpty())
            candidate = format.value(QStringLiteral("url")).toString();
        if (candidate.isEmpty())
            continue;

        if (hlsAny.isEmpty())
            hlsAny = candidate;

        const QString vcodec = format.value(QStringLiteral("vcodec")).toString();
        if (hlsAvc.isEmpty()
            && (vcodec.startsWith(QLatin1String("avc"), Qt::CaseInsensitive)
                || vcodec.startsWith(QLatin1String("h264"), Qt::CaseInsensitive))) {
            hlsAvc = candidate;
        }
    }
    if (!hlsAvc.isEmpty())
        return hlsAvc;
    if (!hlsAny.isEmpty())
        return hlsAny;

    // ---- 退路:挑一个"画面和声音在同一个流里"的格式 ----
    //
    // **指定了档位就要求高度正好等于档位**:高清基本是音视频分离的,
    // 合一的只有低清 —— 放宽成"不超过"的话,用户选 1080P 会静默变成
    // 秒开 360P。宁可返回空、老老实实走下载合并(用户选哪档就该拿到哪档)。
    QString bestUrl;
    int bestHeight = -1;

    for (const QJsonValue &value : formats) {
        const QJsonObject format = value.toObject();

        const QString vcodec = format.value(QStringLiteral("vcodec")).toString();
        const QString acodec = format.value(QStringLiteral("acodec")).toString();
        // 检测到纯视频或纯音频
        if (vcodec == QLatin1String("none") || acodec == QLatin1String("none"))
            continue;

        const QString url = format.value(QStringLiteral("url")).toString();
        if (url.isEmpty())
            continue;

        // 分辨率高的优先;直链文件没有 height 字段(是 null),按 0 算也能选中
        const int height = format.value(QStringLiteral("height")).toInt(0);
        if (heightCap > 0 && height != heightCap)
            continue;

        if (height > bestHeight) {
            bestHeight = height;
            bestUrl = url;
        }
    }

    return bestUrl;
}

// 从 yt-dlp 的输出里挑"原因"那一行。它出错时总是打一行 ERROR: 开头的话,
// 而周围混着别的东西(合并通道把 stderr 也并进来了,`-J` 失败时还会单独
// 输出一个 null)—— 整段原样显示会带一堆噪声,只挑 ERROR 那行最清楚。
// 挑不到(比如根本不是 yt-dlp 的报错)就退回整段原文。
QString errorFromOutput(const QStringList &output, const QString &fallback)
{
    for (int i = output.size() - 1; i >= 0; --i) {
        const QString line = output.at(i).trimmed();
        if (line.startsWith(QLatin1String("ERROR:")))
            return line;
    }
    return output.isEmpty() ? fallback : output.join(QLatin1Char('\n'));
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

void UrlDownloader::removeItemTempDir(const QString &roomId, const QString &clientId,
                                      const QString &itemId)
{
    QDir(itemTempDir(roomId, clientId, itemId)).removeRecursively();
}

void UrlDownloader::removeRoomTempDir(const QString &roomId, const QString &clientId)
{
    QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)
         + QStringLiteral("/syncine/") + sanitize(clientId)
         + QLatin1Char('/') + sanitize(roomId)).removeRecursively();
}

void UrlDownloader::cancelAndRemoveDir(const QString &dir)
{
    if (m_process == nullptr) {
        QDir(dir).removeRecursively(); // 没有在跑的进程,直接删
        return;
    }

    m_removeDirAfterExit = dir;
    m_process->kill();
}

// ============================
// 探测
// ============================

void UrlDownloader::probe(const QString &url, int heightCap)
{
    m_heightCap = heightCap;
    startProbe(url, /*inspectOnly=*/false);
}

void UrlDownloader::inspect(const QString &url)
{
    // 只是给人看一眼有哪些档位,不涉及直链,所以档位上限无意义
    m_heightCap = 0;
    startProbe(url, /*inspectOnly=*/true);
}

void UrlDownloader::startProbe(const QString &url, bool inspectOnly)
{
    if (m_process != nullptr) {
        LOG_DEBUG("UrlDownloader") << "上一次任务还没结束,忽略这次解析";
        return;
    }
    if (m_toolchain == nullptr || !m_toolchain->ytDlpReady()) {
        fail(QStringLiteral("找不到 yt-dlp,无法解析链接"));
        return;
    }

    m_url = url;
    m_inspectOnly = inspectOnly;
    m_title.clear();
    m_uploader.clear();
    m_durationMs = 0;
    m_directUrl.clear();
    m_qualities.clear();
    m_error.clear();
    m_progress = -1;
    m_detail.clear();
    setStage(QStringLiteral("probing"));

    startProcess({QStringLiteral("-J"), QStringLiteral("--no-playlist")}, url);
}

void UrlDownloader::handleProbeFinished()
{
    const QByteArray output = m_outputBuffer;
    // 探测的 stdout 就是那一整份 JSON(合并通道后它可能和警告混在一起,
    // 所以从第一个 '{' 开始截)
    const int start = output.indexOf('{');
    const QJsonDocument document = start >= 0
        ? QJsonDocument::fromJson(output.mid(start))
        : QJsonDocument();
    if (!document.isObject()) {
        fail(errorFromOutput(m_outputTail, QStringLiteral("yt-dlp 没能解析这个链接")));
        return;
    }

    const QJsonObject root = document.object();
    m_title = root.value(QStringLiteral("title")).toString();
    // uploader 是通用字段,频道类的站点给 channel,拿不到就算了(界面会少显示一段)
    m_uploader = root.value(QStringLiteral("uploader")).toString();
    if (m_uploader.isEmpty())
        m_uploader = root.value(QStringLiteral("channel")).toString();

    const QJsonArray formats = root.value(QStringLiteral("formats")).toArray();
    const double seconds = root.value(QStringLiteral("duration")).toDouble(0);
    m_durationMs = static_cast<qint64>(seconds * 1000);

    // ---- 直播判定 ----
    // yt-dlp 的 is_live 只在"提取器认识这个站点"时才准;裸 m3u8 链接走通用
    // 提取器时它常常是 null。补一条:是 HLS 又**算不出总时长** —— 直播清单没有
    // EXT-X-ENDLIST,推不出时长,这个特征足够可靠(实测 VOD 的 HLS 能算出时长)。
    m_isLive = root.value(QStringLiteral("is_live")).toBool(false);
    if (!m_isLive && m_durationMs <= 0 && isHls(formats))
        m_isLive = true;

    // 画质档位:两条路都要(弹窗拿它给用户挑,播放路径也要知道自己被限在哪一档)
    m_qualities = qualityOptionsFromFormats(formats, m_isLive);

    if (m_inspectOnly) {
        // 只是"看一看":不挑直链、也不试拉 —— 那一步最多要等 8 秒,
        // 而弹窗这会儿只需要标题/时长/档位列表
        m_directUrl.clear();
        finishProbe();
        return;
    }

    QString rejectReason;
    const QString candidate = pickDirectFormat(formats, m_isLive, m_heightCap, &rejectReason);

    if (candidate.isEmpty() || m_network == nullptr) {
        m_directUrl.clear();
        m_verifyRejectReason = rejectReason;
        finishProbe();
        return;
    }

    // 有候选:**实际试一下**再决定能不能直连(异步,完了走 finishProbe)
    m_verifying = true;
    m_verifyCandidate = candidate;
    m_verifyRejectReason.clear();
    verifyDirectUrl(candidate);
}

void UrlDownloader::verifyDirectUrl(const QString &url)
{
    QNetworkRequest request{QUrl(url)};
    // 只要第一个字节:既验证了可达性,又不会把整个媒体拉下来。
    // **不带任何自定义头** —— 要测的就是"播放器那种处境下能不能拉"
    // (QMediaPlayer 加不了 Referer/Cookie)。
    request.setRawHeader("Range", "bytes=0-0");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(8000);

    QNetworkReply *reply = m_network->get(request);
    m_verifyReply = reply;
    auto *gotData = new bool(false);

    connect(reply, &QNetworkReply::readyRead, this, [reply, gotData]() {
        // 能拉就立刻断:直播流会一直吐字节,不能等它结束(直播永远不会结束)。
        //
        // **先看状态码**:403/404 的响应体是 HTML 错误页,它同样会触发 readyRead ——
        // 只看"有没有数据"会把"被拒绝"当成"能拉"(实测踩过)。
        if (*gotData)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status < 200 || status >= 300)
            return;
        *gotData = true;
        reply->abort();
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, gotData]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = *gotData || status == 200 || status == 206;
        delete gotData;
        reply->deleteLater();
        m_verifyReply = nullptr;
        m_verifying = false;

        if (ok) {
            LOG_INFO("UrlDownloader") << "直链可直接拉取(HTTP" << status << ")";
            m_directUrl = m_verifyCandidate;
        } else {
            m_directUrl.clear();
            m_verifyRejectReason =
                QStringLiteral("直链拉不动(HTTP %1),通常要 Referer/Cookie,"
                               "播放器加不了请求头(导入了 cookie 也一样 —— "
                               "cookie 只作用于下载那条路)")
                    .arg(status > 0 ? QString::number(status) : QStringLiteral("连接失败"));
            LOG_WARN("UrlDownloader") << "直链试拉失败: HTTP" << status << reply->errorString();
        }
        finishProbe();
    });
}

void UrlDownloader::finishProbe()
{
    if (m_inspectOnly) {
        QStringList labels;
        for (const QVariant &option : m_qualities)
            labels << option.toMap().value(QStringLiteral("label")).toString();
        LOG_INFO("UrlDownloader") << "解析完成(只看画质):" << m_title
                                  << QStringLiteral("档位[%1]").arg(labels.join(QLatin1Char('/')));
    } else if (m_directUrl.isEmpty()) {
        LOG_INFO("UrlDownloader") << (m_isLive ? "直播源没有可直接播放的流:"
                                               : "没有可直连的格式,这条将走下载合并:")
                                  << (m_verifyRejectReason.isEmpty() ? m_title
                                                                     : m_verifyRejectReason);
    } else {
        LOG_INFO("UrlDownloader") << (m_isLive ? "直播源,可直链播放:"
                                               : "可以直链播放:") << m_title;
    }

    // 停在 "probed" 而不是回 idle:界面靠它显示"解析成功、等你点加入"
    setStage(QStringLiteral("probed"));
    emit probed();
}

// ============================
// 下载
// ============================

void UrlDownloader::download(const QString &url, const QString &destDir, int heightCap)
{
    if (m_process != nullptr) {
        LOG_DEBUG("UrlDownloader") << "上一次任务还没结束,忽略这次下载";
        return;
    }
    m_heightCap = heightCap;
    if (m_toolchain == nullptr || !m_toolchain->ytDlpReady()) {
        fail(QStringLiteral("找不到 yt-dlp,无法下载"));
        return;
    }

    m_destDir = destDir;
    m_downloadedBytes = 0;
    m_totalBytes = 0;
    m_speedBps = 0;
    m_etaSec = -1;
    m_progress = -1;
    m_error.clear();
    m_outputTail.clear();
    QDir().mkpath(destDir);
    // 先把目录清干净再下:上一次失败留下的中间文件(经常是纯音频那条流)
    // 会被后面的产物查找误认成结果
    const QDir destDirInfo(destDir);
    for (const QString &name : destDirInfo.entryList({QStringLiteral("media.*")}, QDir::Files))
        QFile::remove(destDirInfo.absoluteFilePath(name));
    setStage(QStringLiteral("downloading"));

    // 输出名固定成 media.<ext>:扩展名事先不知道(要看源格式,合并后是 mkv),
    // 结束时在目录里 glob "media.*" 就能拿到。
    const QString outputTemplate = destDir + QStringLiteral("/media.%(ext)s");
    const QString progress = QStringLiteral(
        "download:%(progress.downloaded_bytes)s %(progress.total_bytes)s "
        "%(progress.total_bytes_estimate)s %(progress.speed)s %(progress.eta)s");

    // 档位上限:0 = 最高(yt-dlp 自己挑,和没有这个功能之前一样)。
    // `bv*+ba/b` 是 yt-dlp 的默认选择表达式:优先"最佳画面 + 最佳声音",
    // 站点只有合一格式时退回单条 —— 加上 [height<=N] 就是"不超过这个高度"。
    const QString format = heightCap > 0
        ? QStringLiteral("bv*[height<=%1]+ba/b[height<=%1]").arg(heightCap)
        : QStringLiteral("bv*+ba/b");

    startProcess({QStringLiteral("-f"), format,
                  QStringLiteral("--merge-output-format"), QStringLiteral("mkv"),
                  QStringLiteral("--newline"),
                  QStringLiteral("--progress-template"), progress,
                  QStringLiteral("--no-playlist"),
                  QStringLiteral("-o"), outputTemplate},
                 url);
}

void UrlDownloader::handleDownloadLine(const QString &line)
{
    // yt-dlp 在合并时会打 "[Merger] Merging formats into ..." ——
    // 这一段没有任何进度输出,不告诉用户的话界面会卡在 100% 不动
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
        // 总大小经常没有(DASH/HLS 事先不知道),回退到估算值,否则算不出百分比
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
        fail(errorFromOutput(m_outputTail,
                             QStringLiteral("yt-dlp 退出码 %1").arg(exitCode)));
        return;
    }

    // 找产物:**只认最终的那个 media.<ext>**(合并完是 mkv,单文件格式保持原扩展名)。
    //
    // 中间流文件的形状是 media.<format_id>.<ext>,比如 media.f30280.m4a 是**纯音频** ——
    // 上一轮失败留下的这种文件如果被当成产物加载,症状就是"只有声音"(实测踩过)。
    const QDir dir(m_destDir);
    const QStringList entries = dir.entryList({QStringLiteral("media.*")}, QDir::Files);
    for (const QString &name : entries) {
        // "media." 之后还有一层点 = 中间流文件或未完成文件,都不是产物
        if (name.mid(6).contains(QLatin1Char('.')))
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
    // "试拉结束"、拿着已经作废的候选走 finishProbe
    disconnect(m_verifyReply, nullptr, this, nullptr);
    m_verifyReply->abort();
    m_verifyReply->deleteLater();
    m_verifyReply = nullptr;
    m_verifying = false;
    m_verifyCandidate.clear();
}

void UrlDownloader::startProcess(const QStringList &options, const QString &url)
{
    abortVerify(); // 新任务开始,旧任务的试拉结果一律作废

    m_outputTail.clear();
    m_outputBuffer.clear();

    m_process = new QProcess(this);
    // 合并 stdout/stderr:进度和错误信息在一个流里,顺序也真实
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setProgram(m_toolchain->ytDlpPath());


    // 拼接参数:用户选项 + ffmpeg-location + cookies + url
    // **url 必须留在最后一位**,前面这些谁先谁后都无所谓
    QStringList args = options;
    args << m_toolchain->ffmpegLocationArgs();
    if (m_cookies != nullptr)
        args << m_cookies->cookieArgs();
    args << url;
    m_process->setArguments(args);

    // 打**拼好的完整参数**(不只 options)—— "为什么没带上 cookie / 用的是哪个
    // ffmpeg" 这类问题,看一眼这行就清楚了
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

                // cancelAndRemoveDir 交代的事:进程真的退出了才删目录
                if (!m_removeDirAfterExit.isEmpty()) {
                    QDir(m_removeDirAfterExit).removeRecursively();
                    m_removeDirAfterExit.clear();
                }

                // cancel() 会把阶段设回 idle —— 这种"异常结束"是用户自己取消的,
                // 不能报成失败
                if (stage != QLatin1String("probing")
                    && stage != QLatin1String("downloading")
                    && stage != QLatin1String("merging")) {
                    return;
                }

                if (status != QProcess::NormalExit) {
                    fail(QStringLiteral("yt-dlp 异常结束"));
                    return;
                }

                if (stage == QLatin1String("probing"))
                    handleProbeFinished();
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

    // 复位:界面(加条目弹窗)关掉/重开时要回到干净状态。
    // **没有进程在跑时也要清** —— 上一次解析成功留下的标题、档位列表
    // 不清掉的话,下次打开弹窗会直接看到上一次的结果。
    //
    // 这里**只发 stateChanged**:setStage 对相同取值去重,而 reset 之后
    // 各属性都变了,得让绑定有机会重算。千万别在这里 emit probed() ——
    // PlaybackSync::retryCurrentItem 是先 cancel 再重新准备,
    // 一个多余的 probed 会让它拿着刚被清空的直链直接去下载。
    m_progress = -1;
    m_detail.clear();
    m_error.clear();
    m_title.clear();
    m_uploader.clear();
    m_durationMs = 0;
    m_directUrl.clear();
    m_isLive = false;
    m_qualities.clear();
    m_inspectOnly = false;
    m_heightCap = 0;
    const bool stageChanged = m_stage != QLatin1String("idle");
    setStage(QStringLiteral("idle"));
    // 本来就是 idle 时 setStage 会去重(不发信号),可上面那一串清空同样
    // 需要让绑定重算 —— 补一次,别让界面留着上次的数据
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
