#include "FormatModel.h"

#include <algorithm>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QUrl>

#include "core/Log.h"

namespace {

// ---- 数值兜底 ----
// yt-dlp 的 JSON 中这些字段可能为 null（取不到），toDouble/toInteger 会当作 0。
// 但「filesize 为 0」与「没有 filesize」在界面上需要区分，故统一收进一个字段再判断。

// yt-dlp 的 JSON 里同一字段可能是数字、字符串或 null；插件尤其容易给字符串。
QString stringOf(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (number == static_cast<double>(static_cast<qint64>(number)))
            return QString::number(static_cast<qint64>(number));
        return QString::number(number);
    }
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    return {};
}

double doubleOf(const QJsonValue &value)
{
    if (value.isDouble())
        return value.toDouble();
    if (value.isString()) {
        const QString text = value.toString().trimmed();
        if (text.isEmpty() || text.compare(QLatin1String("NA"), Qt::CaseInsensitive) == 0)
            return 0;
        bool ok = false;
        const double number = text.toDouble(&ok);
        return ok ? number : 0;
    }
    return 0;
}

int intOf(const QJsonValue &value)
{
    return static_cast<int>(doubleOf(value));
}

qint64 int64Of(const QJsonValue &value)
{
    return static_cast<qint64>(doubleOf(value));
}

bool boolOf(const QJsonValue &value)
{
    if (value.isBool())
        return value.toBool();
    if (value.isDouble())
        return value.toDouble() != 0;
    if (value.isString()) {
        const QString text = value.toString().trimmed().toLower();
        return text == QLatin1String("true") || text == QLatin1String("1")
               || text == QLatin1String("yes");
    }
    return false;
}

// vcodec/acodec 在 yt-dlp 的 JSON 中是字符串，"none" 表示该路不存在。
// null/缺失表示"未知"（generic 直链常见），不能当成 none —— 否则
// TikTok CDN 这类只有 URL、没有编码信息的直链会被整个丢掉。
bool hasCodec(const QJsonValue &value)
{
    const QString text = stringOf(value);
    return !text.isEmpty() && text != QLatin1String("none");
}

// 源地址本身是不是 HLS 清单。只能用它判断“裸 m3u8 直播”，
// 不能因为 formats 里有 m3u8 就判直播 —— 插件把点播 HLS 作为普通格式返回时，
// duration 也可能为空。
bool looksLikeHlsManifest(const QString &url)
{
    if (url.isEmpty())
        return false;
    return QUrl(url).path().endsWith(QLatin1String(".m3u8"), Qt::CaseInsensitive);
}

bool looksLikeDashManifest(const QString &url)
{
    if (url.isEmpty())
        return false;
    return QUrl(url).path().endsWith(QLatin1String(".mpd"), Qt::CaseInsensitive);
}

// duration 可能是数字、数字字符串，或只有 duration_string（"1:02:03"）。
double durationSecondsOf(const QJsonObject &object)
{
    const double direct = doubleOf(object.value(QStringLiteral("duration")));
    if (direct > 0)
        return direct;

    const QString text =
        stringOf(object.value(QStringLiteral("duration_string"))).trimmed();
    if (text.isEmpty())
        return 0;

    double seconds = 0;
    const QStringList parts = text.split(QLatin1Char(':'));
    for (const QString &part : parts) {
        bool ok = false;
        const double value = part.trimmed().toDouble(&ok);
        if (!ok)
            return 0;
        seconds = seconds * 60 + value;
    }
    return seconds > 0 ? seconds : 0;
}

// 解析 "1920x1080" / "1920x1080, 1080p" / "1080p" 这类 resolution。
bool applyResolutionText(const QString &text, int *width, int *height)
{
    if (text.isEmpty())
        return false;

    static const QRegularExpression pair(
        QStringLiteral("(\\d+)\\s*[xX×]\\s*(\\d+)"));
    QRegularExpressionMatch match = pair.match(text);
    if (match.hasMatch()) {
        *width = match.captured(1).toInt();
        *height = match.captured(2).toInt();
        return *height > 0;
    }

    static const QRegularExpression heightOnly(
        QStringLiteral("(?:^|[^0-9])(\\d{3,4})\\s*[pP](?:[^a-zA-Z]|$)"));
    match = heightOnly.match(text);
    if (match.hasMatch()) {
        *width = 0;
        *height = match.captured(1).toInt();
        return *height > 0;
    }
    return false;
}

// ---- 编码家族 ----
// 把 vcodec/acodec 的完整串（"avc1.640032" / "hvc1.1.6.L150.90" / "mp4a.40.2"）
// 归纳为用户可识别的名字。无法归类时原样返回，不做硬编码猜测。
QString videoCodecName(const QString &vcodec)
{
    if (vcodec.isEmpty() || vcodec == QLatin1String("none"))
        return {};
    const QString text = vcodec.toLower();
    if (text.startsWith(QLatin1String("avc")) || text.startsWith(QLatin1String("h264")))
        return QStringLiteral("H.264");
    if (text.startsWith(QLatin1String("hev")) || text.startsWith(QLatin1String("hvc"))
        || text.startsWith(QLatin1String("hevc"))) {
        return QStringLiteral("HEVC");
    }
    if (text.startsWith(QLatin1String("av01")))
        return QStringLiteral("AV1");
    if (text.startsWith(QLatin1String("vp09")) || text.startsWith(QLatin1String("vp9")))
        return QStringLiteral("VP9");
    if (text.startsWith(QLatin1String("avs")))
        return QStringLiteral("AVS");
    if (text.startsWith(QLatin1String("mpeg")))
        return QStringLiteral("MPEG");
    return vcodec;
}

QString audioCodecName(const QString &acodec)
{
    if (acodec.isEmpty() || acodec == QLatin1String("none"))
        return {};
    const QString text = acodec.toLower();
    if (text.startsWith(QLatin1String("mp4a")) || text.startsWith(QLatin1String("aac")))
        return QStringLiteral("AAC");
    if (text.startsWith(QLatin1String("opus")))
        return QStringLiteral("Opus");
    if (text.startsWith(QLatin1String("vorbis")))
        return QStringLiteral("Vorbis");
    if (text.startsWith(QLatin1String("mp3")))
        return QStringLiteral("MP3");
    if (text.startsWith(QLatin1String("flac")))
        return QStringLiteral("FLAC");
    if (text.startsWith(QLatin1String("ec-3")) || text.startsWith(QLatin1String("ac-3")))
        return QStringLiteral("AC-3");
    if (text.startsWith(QLatin1String("alac")))
        return QStringLiteral("ALAC");
    return acodec;
}

QString channelsName(int channels)
{
    switch (channels) {
    case 1: return QStringLiteral("单声道");
    case 2: return QStringLiteral("立体声");
    case 3: return QStringLiteral("2.1");
    case 6: return QStringLiteral("5.1");
    case 8: return QStringLiteral("7.1");
    default: return channels > 0 ? QStringLiteral("%1 声道").arg(channels) : QString();
    }
}

QString bitrateText(double kbps)
{
    if (kbps <= 0)
        return {};
    if (kbps >= 1000)
        return QStringLiteral("%1 Mbps").arg(kbps / 1000.0, 0, 'f', 1);
    return QStringLiteral("%1 kbps").arg(qRound(kbps));
}

QString sizeText(qint64 bytes)
{
    if (bytes <= 0)
        return {};
    constexpr double kMiB = 1024.0 * 1024.0;
    if (bytes >= 1024LL * 1024 * 1024)
        return QStringLiteral("%1 GB").arg(bytes / (kMiB * 1024), 0, 'f', 2);
    return QStringLiteral("%1 MB").arg(bytes / kMiB, 0, 'f', 1);
}

QString resolutionText(const MediaFormat &format)
{
    if (format.height <= 0)
        return {};
    const QString base = format.width > 0
        ? QStringLiteral("%1x%2").arg(format.width).arg(format.height)
        : QStringLiteral("%1P").arg(format.height);
    if (format.fps >= 1)
        return QStringLiteral("%1 · %2fps").arg(base).arg(format.fps, 0, 'f', format.fps < 10 ? 2 : 0);
    return base;
}

// 分辨率档的标签。仅标出 60 这类整数帧率，其余不标（24/25/30 标出即为噪声）
QString tierLabel(int height, double fps)
{
    QString label = QStringLiteral("%1P").arg(height);
    if (fps >= 50)
        label += QStringLiteral("60");
    return label;
}

// ---- 动态范围 ----
// yt-dlp 的 dynamic_range 通常为 "SDR"；HDR 为 "HDR"/"HLG"，杜比视界为 "DV"。
// 部分站点返回 null，此时按 SDR 处理，避免误报。
bool dynamicRangeIsHdr(const QString &range)
{
    if (range.isEmpty() || range.compare(QLatin1String("SDR"), Qt::CaseInsensitive) == 0)
        return false;
    const QString text = range.toLower();
    return text.contains(QLatin1String("hdr")) || text.contains(QLatin1String("hlg"))
           || text == QLatin1String("dv") || text.contains(QLatin1String("dolby"))
           || text.contains(QLatin1String("dovi"));
}

// 编码家族优先级：同分辨率下 h264 优先。理由：兼容性最好（Qt 与后续推流均可解码）；
// 且 B 站的 avc 档通常就是高码率档（实测 1080P：30112 avc ≈3Mbps，30102 hevc ≈1.5Mbps），
// 码率更高则画质更好。调整偏好只需修改本函数中的顺序。
int vcodecRank(const QString &vcodec)
{
    const QString name = videoCodecName(vcodec);
    if (name == QLatin1String("H.264")) return 0;
    if (name == QLatin1String("HEVC"))  return 1;
    if (name == QLatin1String("AV1"))   return 2;
    if (name == QLatin1String("VP9"))   return 3;
    return 4;
}

// 音频：同档位下 AAC 优先（兼容性考虑），其次 Opus/Vorbis
int acodecRank(const QString &acodec)
{
    const QString name = audioCodecName(acodec);
    if (name == QLatin1String("AAC"))    return 0;
    if (name == QLatin1String("Opus"))   return 1;
    if (name == QLatin1String("Vorbis")) return 2;
    return 3;
}

bool isBetterKind(MediaFormat::Kind a, MediaFormat::Kind b)
{
    // 纯画面优于合一格式：合一格式不含高清（高清需分流），且精确获取两路更可控
    auto rank = [](MediaFormat::Kind kind) {
        switch (kind) {
        case MediaFormat::VideoOnly: return 0;
        case MediaFormat::Combined:  return 1;
        case MediaFormat::AudioOnly: return 2;
        case MediaFormat::Unknown:   return 3;
        }
        return 3;
    };
    return rank(a) < rank(b);
}

// 按「简单模式的优先级」比较两条流：分辨率 → 帧率 → 编码 → 种类。
// 这是 `-S` 排序规则在本地的一份镜像（弹窗展示、手选兜底都用它）。
bool preferenceBetter(const MediaFormat &a, const MediaFormat &b)
{
    if (a.height != b.height)
        return a.height > b.height;
    if (a.width != b.width)
        return a.width > b.width;
    if (static_cast<int>(a.fps) != static_cast<int>(b.fps))
        return a.fps > b.fps;
    if (vcodecRank(a.vcodec) != vcodecRank(b.vcodec))
        return vcodecRank(a.vcodec) < vcodecRank(b.vcodec);
    if (acodecRank(a.acodec) != acodecRank(b.acodec))
        return acodecRank(a.acodec) < acodecRank(b.acodec);
    if (isBetterKind(a.kind, b.kind) != isBetterKind(b.kind, a.kind))
        return isBetterKind(a.kind, b.kind);
    return a.formatId < b.formatId; // 兜底：保证结果稳定，重跑一致
}

// 在 candidates 中挑选优先级最高的一条；allowHdr=false 时只从非 HDR 中挑选
MediaFormat bestOf(const QList<MediaFormat> &candidates, bool allowHdr)
{
    MediaFormat best;
    bool have = false;
    for (const MediaFormat &format : candidates) {
        if (!allowHdr && format.isHdr())
            continue;
        if (!have || preferenceBetter(format, best)) {
            best = format;
            have = true;
        }
    }
    return best;
}

QList<MediaFormat> filterKind(const QList<MediaFormat> &formats, MediaFormat::Kind kind)
{
    QList<MediaFormat> result;
    for (const MediaFormat &format : formats) {
        if (format.kind == kind)
            result.append(format);
    }
    return result;
}

// 没有 codec 信息时，用 video_ext/audio_ext 和容器扩展名推断媒体类型。
MediaFormat::Kind inferKind(bool hasVideo, bool hasAudio, bool bothExplicitNone,
                            const QString &videoExt, const QString &audioExt,
                            const QString &ext, const QString &sourceUrl)
{
    if (hasVideo && hasAudio)
        return MediaFormat::Combined;
    if (hasVideo)
        return MediaFormat::VideoOnly;
    if (hasAudio)
        return MediaFormat::AudioOnly;
    if (bothExplicitNone)
        return MediaFormat::Unknown;

    const bool hasSource = !sourceUrl.isEmpty();
    const bool videoNone = videoExt == QLatin1String("none");
    const bool audioNone = audioExt == QLatin1String("none");

    static const QStringList audioExts{
        QStringLiteral("m4a"), QStringLiteral("mp3"), QStringLiteral("opus"),
        QStringLiteral("ogg"), QStringLiteral("oga"), QStringLiteral("aac"),
        QStringLiteral("flac"), QStringLiteral("wav"),
    };
    const bool audioContainer = audioExts.contains(ext.toLower());

    if (videoNone && audioNone) {
        // 两个 ext 都明确 none：一般不是媒体；音频容器属于少见但可用的例外
        return hasSource && audioContainer ? MediaFormat::AudioOnly
                                           : MediaFormat::Unknown;
    }
    if (!videoNone && audioNone)
        return hasSource ? MediaFormat::Combined : MediaFormat::Unknown;
    if (videoNone && !audioNone)
        return hasSource ? MediaFormat::AudioOnly : MediaFormat::Unknown;

    // video_ext/audio_ext 都缺失：先按容器名判断音频，否则沿用
    // “generic MP4 / HLS variant 可能带音轨”的旧行为，按合一处理。
    if (!hasSource)
        return MediaFormat::Unknown;
    return audioContainer ? MediaFormat::AudioOnly : MediaFormat::Combined;
}

// 自定义 extractor 可能不写 protocol；按 URL 后缀/协议补上。
void inferProtocol(MediaFormat *format)
{
    if (format == nullptr || !format->protocol.isEmpty())
        return;

    const QString sourceUrl = format->url.isEmpty() ? format->manifestUrl
                                                    : format->url;
    if (looksLikeHlsManifest(sourceUrl)) {
        format->protocol = QStringLiteral("m3u8_native");
        return;
    }
    if (looksLikeDashManifest(sourceUrl)) {
        format->protocol = QStringLiteral("http_dash_segments");
        return;
    }
    const QString scheme = QUrl(sourceUrl).scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        format->protocol = scheme;
}

// 把 formats[] 的一项归一化为 MediaFormat；不能识别时返回 valid=false。
MediaFormat parseFormatObject(const QJsonObject &object)
{
    MediaFormat format;

    const QJsonValue vcodecValue = object.value(QStringLiteral("vcodec"));
    const QJsonValue acodecValue = object.value(QStringLiteral("acodec"));
    const QString vcodec = stringOf(vcodecValue);
    const QString acodec = stringOf(acodecValue);
    const bool hasVideo = hasCodec(vcodecValue);
    const bool hasAudio = hasCodec(acodecValue);
    const bool bothExplicitNone =
        vcodec == QLatin1String("none") && acodec == QLatin1String("none");

    const QString url = stringOf(object.value(QStringLiteral("url")));
    const QString manifestUrl =
        stringOf(object.value(QStringLiteral("manifest_url")));
    const QString sourceUrl = url.isEmpty() ? manifestUrl : url;
    const QString videoExt = stringOf(object.value(QStringLiteral("video_ext")));
    const QString audioExt = stringOf(object.value(QStringLiteral("audio_ext")));
    const QString ext = stringOf(object.value(QStringLiteral("ext")));

    const MediaFormat::Kind kind = inferKind(hasVideo, hasAudio, bothExplicitNone,
                                             videoExt, audioExt, ext, sourceUrl);
    if (kind == MediaFormat::Unknown)
        return format;

    format.formatId = stringOf(object.value(QStringLiteral("format_id")));
    format.url = url;
    format.protocol = stringOf(object.value(QStringLiteral("protocol")));
    format.manifestUrl = manifestUrl;
    inferProtocol(&format);

    const QString formatNote = stringOf(object.value(QStringLiteral("format_note")));
    format.width = intOf(object.value(QStringLiteral("width")));
    format.height = intOf(object.value(QStringLiteral("height")));
    if (format.width <= 0 || format.height <= 0) {
        if (!applyResolutionText(stringOf(object.value(QStringLiteral("resolution"))),
                                 &format.width, &format.height)) {
            applyResolutionText(formatNote, &format.width, &format.height);
        }
    }

    format.fps = doubleOf(object.value(QStringLiteral("fps")));
    format.vcodec = vcodec;
    format.acodec = acodec;
    format.vbr = doubleOf(object.value(QStringLiteral("vbr")));
    format.abr = doubleOf(object.value(QStringLiteral("abr")));
    format.tbr = doubleOf(object.value(QStringLiteral("tbr")));
    format.filesize = int64Of(object.value(QStringLiteral("filesize")));
    format.filesizeApprox = int64Of(object.value(QStringLiteral("filesize_approx")));
    format.audioChannels = intOf(object.value(QStringLiteral("audio_channels")));
    format.language = stringOf(object.value(QStringLiteral("language")));
    format.dynamicRange = stringOf(object.value(QStringLiteral("dynamic_range")));
    format.formatNote = formatNote;
    format.quality = intOf(object.value(QStringLiteral("quality")));
    format.ext = ext;

    format.kind = kind;
    format.valid = true;
    return format;
}

// 按分辨率档挑画面：height 取自 formats 的 height 字段。
//
// 统一规则：height > 0 时只考虑“不超过该高度”的流；height = 0 时不设限。
// 关闭 HDR 时再排除 HDR。这样 1080P 档可以退到 720P SDR，也不会在找不到
// 精确高度时反而选到更高的 2160P；找不到就返回空，由上层报错。
MediaFormat pickVideoTier(const MediaInfo &info, int height, bool allowHdr)
{
    QList<MediaFormat> candidates;
    for (const MediaFormat &format : info.formats) {
        if (!format.isVideo() || format.height <= 0)
            continue;
        if (height > 0 && format.height > height)
            continue;
        if (!allowHdr && format.isHdr())
            continue;
        candidates.append(format);
    }

    const MediaFormat best = bestOf(candidates, allowHdr);
    if (!best.valid) {
        LOG_DEBUG("Format") << "档位无可用画面流: height=" << height
                            << " 候选=" << candidates.size()
                            << " 源内视频流=" << filterKind(info.formats, MediaFormat::VideoOnly).size()
                            << " 合一=" << filterKind(info.formats, MediaFormat::Combined).size();
    }
    return best;
}

MediaFormat pickBestAudio(const MediaInfo &info, bool allowHdr)
{
    const MediaFormat best = bestOf(filterKind(info.formats, MediaFormat::AudioOnly), allowHdr);
    if (!best.valid)
        LOG_DEBUG("Format") << "源内没有可用音频流，将依赖 yt-dlp 的 bv*+ba/b 兜底";
    return best;
}

// 直播的清晰度只能依据 yt-dlp 的 quality 序号（实测 B 站：原画=4 > 蓝光=3 > 超清=2）。
// 部分站点会提供 height，更准确，优先使用。
int liveRank(const MediaFormat &format)
{
    return format.height > 0 ? format.height : format.quality;
}

bool liveBetter(const MediaFormat &a, const MediaFormat &b)
{
    const int rankA = liveRank(a);
    const int rankB = liveRank(b);
    if (rankA != rankB)
        return rankA > rankB;
    // 同一清晰度有多条时（不同协议/编码）：
    // m3u8 优先——Qt 播放器可直接拉取 HLS；FLV/DASH 不行
    const bool hlsA = a.protocol.contains(QLatin1String("m3u8"));
    const bool hlsB = b.protocol.contains(QLatin1String("m3u8"));
    if (hlsA != hlsB)
        return hlsA;
    // 编码优先 avc：实测 B 站直播的 HEVC variant 常需要 Referer，h264 不需要
    const int codecA = vcodecRank(a.vcodec);
    const int codecB = vcodecRank(b.vcodec);
    if (codecA != codecB)
        return codecA < codecB;
    return a.formatId < b.formatId;
}

// 按清晰度分组，挑出每组代表（见 liveBetter）；组号用于去重，组的代表决定顺序
QList<QPair<int, MediaFormat>> liveTiers(const MediaInfo &info)
{
    QList<QPair<int, MediaFormat>> groups;
    int skipped = 0;
    for (const MediaFormat &format : info.formats) {
        // 只考虑能直接拉取的（http(s) / HLS）：FLV、DASH 播放器吃不下
        if (!format.isDirectPlayable()) {
            ++skipped;
            continue;
        }
        if (format.url.isEmpty() && format.manifestUrl.isEmpty())
            continue;
        const int rank = liveRank(format);

        int slot = -1;
        for (int i = 0; i < groups.size(); ++i) {
            if (groups.at(i).first == rank) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            QPair<int, MediaFormat> entry{rank, format};
            groups.append(entry);
            slot = groups.size() - 1;
        } else if (liveBetter(format, groups.at(slot).second)) {
            groups[slot].second = format;
        }
    }

    std::sort(groups.begin(), groups.end(),
              [](const QPair<int, MediaFormat> &a, const QPair<int, MediaFormat> &b) {
                  // 清晰度高的在前；相同则按协议/编码优先级
                  if (a.first != b.first)
                      return a.first > b.first;
                  return liveBetter(a.second, b.second);
              });

    if (groups.isEmpty()) {
        LOG_WARN("Format") << "直播流中没有可直接拉取的地址: 总流数=" << info.formats.size()
                           << " 非直连协议=" << skipped;
    }
    return groups;
}

// 从直播 formats 中挑一条可拉取的 HLS：取清晰度最高档，优先 avc
// （实测 B 站直播 HEVC 那条需要 Referer，h264 不需要）。地址优先 manifest_url
// （master 清单可带 EXT-X-MEDIA、供播放器自适应），为空时用格式自身的 url。
MediaFormat pickLiveHls(const MediaInfo &info)
{
    const QList<QPair<int, MediaFormat>> groups = liveTiers(info);
    MediaFormat fallback; // 无 m3u8 时的退路（FLV 同样拉不动，仅让上层有内容可报错）
    for (const QPair<int, MediaFormat> &group : groups) {
        const MediaFormat &format = group.second;
        if (!format.protocol.contains(QLatin1String("m3u8")))
            continue;
        if (vcodecRank(format.vcodec) == 0) // 已是 avc，优先级最高
            return format;
        if (!fallback.valid)
            fallback = format;
    }
    if (fallback.valid) {
        LOG_WARN("Format") << "直播流中没有 avc 的 HLS，退用非 avc 版本:"
                           << fallback.formatId << fallback.protocol;
        return fallback;
    }

    if (groups.isEmpty()) {
        LOG_ERROR("Format") << "直播流中找不到任何可播放的地址，无法直接播放";
        return MediaFormat();
    }

    LOG_WARN("Format") << "直播流中没有 m3u8，退用清晰度最高的非 HLS 流:"
                       << groups.first().second.protocol;
    return groups.first().second;
}

// 按档位下：选中的流会钉成精确的 `-f` 表达式，保证下载内容与界面标出的
// label/分辨率一致。对方没有这条 id（登录态/会员等级不同）时，下载器会退回
// `-S` 按档位重挑；关闭 HDR 时还会用 ffprobe 复核下载结果。
MediaSelection tierSelection(const MediaInfo &info, int height, bool allowHdr)
{
    const MediaFormat video = pickVideoTier(info, height, allowHdr);

    MediaSelection selection;
    selection.live = false;
    selection.video = video;
    selection.label = video.displayName();

    if (!video.valid) {
        // 没有带 height 的画面流：generic 直链单文件（height/vcodec 都未知）。
        // 选一条音视频合一的直链直接交给播放器；不要再递归。
        // 关闭 HDR 时 bestOf 会跳过 HDR 合一文件；没有 SDR 合一才报错。
        MediaFormat direct = bestOf(filterKind(info.formats, MediaFormat::Combined),
                                    allowHdr);
        const QString target =
            direct.url.isEmpty() ? direct.manifestUrl : direct.url;
        if (direct.valid && !target.isEmpty()
            && direct.isDirectPlayable()) {
            selection.valid = true;
            selection.direct = true;
            selection.video = direct;
            selection.audio = direct;
            selection.url = target;
            selection.label = direct.displayName();
            return selection;
        }

        selection.error = !allowHdr
            ? (height > 0
                   ? QStringLiteral("这个档位没有 SDR 流（设置中已关闭 HDR），"
                                    "请选择更低画质或允许 HDR")
                   : QStringLiteral("这个视频源没有 SDR 流（设置中已关闭 HDR），"
                                    "请允许 HDR 或换一个源"))
            : (height > 0
                   ? QStringLiteral("这个视频源没有不超过该分辨率的画面流，"
                                    "请选择更低画质或不设限")
                   : QStringLiteral("这个视频源没有可供下载的画面流"));
        LOG_WARN("Format") << selection.error
                           << " 候选格式数=" << info.formats.size();
        return selection;
    }

    QStringList sortKeys{
        height > 0 ? QStringLiteral("res:%1").arg(height) : QStringLiteral("res"),
        QStringLiteral("fps"),
        QStringLiteral("vcodec:h264"),
        QStringLiteral("acodec:aac"),
    };
    if (!allowHdr)
        sortKeys.append(QStringLiteral("dynamic_range:sdr"));

    selection.valid = true;
    selection.audio = pickBestAudio(info, allowHdr);
    selection.sortArgs = QStringList{sortKeys.join(QLatin1Char(','))};

    // 精确钉死选中的流。missing id 的兜底在 UrlDownloader::handleDownloadFinished。
    if (video.isCombined()) {
        selection.formatExpression = video.formatId;
    } else if (selection.audio.valid
               && !video.formatId.isEmpty()
               && !selection.audio.formatId.isEmpty()) {
        selection.formatExpression =
            QStringLiteral("%1+%2").arg(video.formatId, selection.audio.formatId);
    }
    // 任一路缺 format_id 时留空，下载器会走 -S 排序；不能拼出 "+audio"

    if (!selection.formatExpression.isEmpty()) {
        LOG_INFO("Format") << "钉死选中的流:" << selection.formatExpression
                           << (allowHdr ? QStringLiteral("(HDR 允许)")
                                         : QStringLiteral("(强制 SDR)"));
    } else {
        LOG_INFO("Format") << "没有完整流对，退回 -S 排序"
                           << (allowHdr
                                   ? QString()
                                   : QStringLiteral("，下载后 ffprobe 复核 HDR"));
    }

    return selection;
}

} // namespace

// ============================
// MediaFormat
// ============================

bool MediaFormat::isHdr() const
{
    return dynamicRangeIsHdr(dynamicRange);
}

QString MediaFormat::videoCodecName() const
{
    return ::videoCodecName(vcodec);
}

QString MediaFormat::audioCodecName() const
{
    return ::audioCodecName(acodec);
}

QString MediaFormat::displayName() const
{
    QStringList parts;

    if (kind == AudioOnly) {
        parts << QStringLiteral("音频");
        if (!audioCodecName().isEmpty())
            parts << audioCodecName();
        const QString bitrate = bitrateText(abr > 0 ? abr : tbr);
        if (!bitrate.isEmpty())
            parts << bitrate;
        const QString channels = channelsName(audioChannels);
        if (!channels.isEmpty())
            parts << channels;
    } else {
        const QString resolution = resolutionText(*this);
        if (!resolution.isEmpty())
            parts << resolution;
        if (!videoCodecName().isEmpty())
            parts << videoCodecName();
        const QString bitrate = bitrateText(tbr > 0 ? tbr : vbr);
        if (!bitrate.isEmpty())
            parts << bitrate;
        if (isHdr())
            parts << QStringLiteral("HDR");
        if (kind == Combined && !audioCodecName().isEmpty())
            parts << QStringLiteral("含 %1 音轨").arg(audioCodecName());
        if (!formatNote.isEmpty() && !formatNote.contains(resolutionText(*this)))
            parts << formatNote;
        // generic 直链可能连分辨率/编码都没有：至少显示容器名，别留空
        if (parts.isEmpty())
            parts << (ext.isEmpty() ? QStringLiteral("视频") : ext.toUpper());
    }

    const QString size = sizeText(bestSize());
    if (!size.isEmpty())
        parts << QStringLiteral("约 %1").arg(size);

    return parts.join(QStringLiteral(" · "));
}

// ============================
// StreamSpec
// ============================

namespace StreamSpec {

bool hdrAllowed(const QString &spec)
{
    return spec.endsWith(QLatin1String(":h"));
}

QString body(const QString &spec)
{
    return hdrAllowed(spec) ? spec.chopped(2) : spec;
}

int heightOf(const QString &spec)
{
    const QString text = body(spec);
    if (text.isEmpty())
        return 0;
    bool ok = false;
    const int value = text.toInt(&ok);
    return ok ? value : -1;
}

QString label(const QString &spec, const MediaInfo &info)
{
    const QString text = body(spec);
    QString base;
    if (text.isEmpty())
        base = QStringLiteral("最高画质");
    else if (text.startsWith(QLatin1String("http")))
        base = QStringLiteral("直播源");
    else if (text.startsWith(QLatin1String("id:"))) {
        // 显式手选：id 可能纯数字，不能当档位显示成 "30080P"。
        const QStringList ids = text.mid(3).split(QLatin1Char('+'));
        int shortSide = 0;
        double fps = 0;
        for (const MediaFormat &format : info.formats) {
            if (ids.contains(format.formatId) && format.isVideo()) {
                shortSide = format.shortSide();
                fps = format.fps;
                break;
            }
        }
        base = shortSide > 0 ? QStringLiteral("手选 %1").arg(tierLabel(shortSide, fps))
                             : QStringLiteral("手选");
    } else if (text.contains(QLatin1Char('+'))) {
        // 手选的 format_id 对：尽量还原出分辨率，无法还原时仅显示「手选」
        const QStringList ids = text.split(QLatin1Char('+'));
        int shortSide = 0;
        double fps = 0;
        for (const MediaFormat &format : info.formats) {
            if (ids.contains(format.formatId) && format.isVideo()) {
                shortSide = format.shortSide();
                fps = format.fps;
                break;
            }
        }
        base = shortSide > 0 ? QStringLiteral("手选 %1").arg(tierLabel(shortSide, fps))
                             : QStringLiteral("手选");
    } else if (text.toInt() > 0) {
        // 档位中存的是 height 字段（竖屏 1080P 存为 "1920"）。要显示为 "1080P"
        // 需知道该流的长宽比，故在 formats 中查找同高度的一条取其短边；
        // 列表侧没有 formats（传入空 info），只能原样显示为 "1920P"。
        //
        // 旧条目的裸数字 format_id（30080/137）也要避免显示成 "30080P"：
        // 能在 formats 里找到同 id，或数字明显超过常见视频高度（>4320）时，
        // 按“手选”显示。新条目都带 id: 前缀，不会走到这里。
        const int height = text.toInt();
        bool isKnownId = false;
        for (const MediaFormat &format : info.formats) {
            if (format.formatId == text) {
                isKnownId = true;
                break;
            }
        }
        if (isKnownId || height > 4320) {
            base = QStringLiteral("手选");
        } else {
            int labelHeight = height;
            for (const MediaFormat &format : info.formats) {
                if (format.height == height && format.isVideo()) {
                    labelHeight = format.shortSide();
                    break;
                }
            }
            base = tierLabel(labelHeight, 0);
        }
    } else {
        base = text; // 无法识别时原样显示
    }

    if (hdrAllowed(spec))
        base += QStringLiteral(" +HDR");
    return base;
}

QString withHdr(const QString &specBody, bool allowHdr)
{
    return allowHdr ? specBody + QStringLiteral(":h") : specBody;
}

} // namespace StreamSpec

// ============================
// 解析
// ============================

namespace MediaFormats {

QList<MediaFormat> parse(const QJsonArray &formats)
{
    QList<MediaFormat> result;
    int skipped = 0;
    for (const QJsonValue &value : formats) {
        if (!value.isObject()) {
            ++skipped;
            continue;
        }
        const MediaFormat format = parseFormatObject(value.toObject());
        if (!format.valid) {
            ++skipped;
            continue;
        }
        result.append(format);
    }

    LOG_DEBUG("Format") << "解析流列表: 输入=" << formats.size()
                        << " 有效=" << result.size()
                        << " 丢弃(非媒体项)=" << skipped;
    return result;
}

MediaInfo parseMediaInfo(const QJsonObject &root)
{
    MediaInfo info;
    const QString rootType = stringOf(root.value(QStringLiteral("_type")));

    // ---- 通用字段：任何一项都可能是 null/缺失 ----
    info.title = stringOf(root.value(QStringLiteral("title")));
    if (info.title.isEmpty())
        info.title = stringOf(root.value(QStringLiteral("fulltitle")));
    if (info.title.isEmpty())
        info.title = stringOf(root.value(QStringLiteral("alt_title")));
    if (info.title.isEmpty())
        info.title = stringOf(root.value(QStringLiteral("display_id")));
    if (info.title.isEmpty())
        info.title = stringOf(root.value(QStringLiteral("id")));

    info.uploader = stringOf(root.value(QStringLiteral("uploader")));
    if (info.uploader.isEmpty())
        info.uploader = stringOf(root.value(QStringLiteral("channel")));
    if (info.uploader.isEmpty())
        info.uploader = stringOf(root.value(QStringLiteral("uploader_id")));
    if (info.uploader.isEmpty())
        info.uploader = stringOf(root.value(QStringLiteral("channel_id")));

    info.url = stringOf(root.value(QStringLiteral("webpage_url")));
    if (info.url.isEmpty())
        info.url = stringOf(root.value(QStringLiteral("original_url")));

    info.durationMs = static_cast<qint64>(durationSecondsOf(root) * 1000);

    info.live = boolOf(root.value(QStringLiteral("is_live")));
    info.wasLive = boolOf(root.value(QStringLiteral("was_live")));
    // 较新的 extractor 更常只给 live_status，is_live 可能缺失。
    const QString liveStatus =
        stringOf(root.value(QStringLiteral("live_status"))).toLower();
    if (!info.live
        && (liveStatus == QLatin1String("is_live")
            || liveStatus == QLatin1String("is_upcoming"))) {
        info.live = true;
    }
    if (!info.wasLive && liveStatus == QLatin1String("was_live"))
        info.wasLive = true;

    info.extractor = stringOf(root.value(QStringLiteral("extractor")));
    if (info.extractor.isEmpty())
        info.extractor = stringOf(root.value(QStringLiteral("extractor_key")));
    if (info.extractor.isEmpty())
        info.extractor = stringOf(root.value(QStringLiteral("ie_key")));
    info.id = stringOf(root.value(QStringLiteral("id")));
    if (info.id.isEmpty())
        info.id = stringOf(root.value(QStringLiteral("display_id")));

    // ---- formats：formats[] → requested_formats[] → 顶层单格式 ----
    QJsonArray formatsArray = root.value(QStringLiteral("formats")).toArray();
    if (formatsArray.isEmpty())
        formatsArray = root.value(QStringLiteral("requested_formats")).toArray();
    info.formats = parse(formatsArray);
    if (info.formats.isEmpty() && rootType != QLatin1String("url")
        && root.value(QStringLiteral("entries")).toArray().isEmpty()) {
        // 单格式信息可能只有顶层 url/format_id，没有 formats[]。
        // 有 entries 的是播放列表，不能把顶层的 url 当成一条视频。
        const MediaFormat topLevel = parseFormatObject(root);
        if (topLevel.valid)
            info.formats.append(topLevel);
    }

    // 顶层 _type=url，或没有 webpage_url 的裸 m3u8：用顶层 url 作为源地址，
    // 供裸 m3u8 直播兜底和相对剧集 URL 解析使用。
    if (info.url.isEmpty()) {
        const QString topUrl = stringOf(root.value(QStringLiteral("url")));
        if (rootType == QLatin1String("url") || looksLikeHlsManifest(topUrl))
            info.url = topUrl;
    }

    // ---- 多集（剧集/合集）：顶层没有 formats，每一集自带 ----
    // 判据仍是「顶层 formats 为空 + entries 非空」。entries 里可能有 null
    // （下架/占位），也可能全是缺少地址的条目；两种情况都要计数并丢弃。
    const QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    if (info.formats.isEmpty() && !entries.isEmpty()) {
        info.playlistTitle = info.title;
        if (info.playlistTitle.isEmpty())
            info.playlistTitle = info.id;
        info.title.clear();   // 顶层 title 是整季的名字，不是某一条的名字
        int dropped = 0;
        for (const QJsonValue &value : entries) {
            if (!value.isObject()) {
                ++dropped;
                continue;
            }
            const QJsonObject entry = value.toObject();

            Episode episode;
            episode.id = stringOf(entry.value(QStringLiteral("id")));
            if (episode.id.isEmpty())
                episode.id = stringOf(entry.value(QStringLiteral("display_id")));
            episode.title = stringOf(entry.value(QStringLiteral("title")));
            if (episode.title.isEmpty())
                episode.title = stringOf(entry.value(QStringLiteral("fulltitle")));
            if (episode.title.isEmpty())
                episode.title = stringOf(entry.value(QStringLiteral("alt_title")));
            if (episode.title.isEmpty())
                episode.title = stringOf(entry.value(QStringLiteral("display_id")));
            // flat playlist（--flat-playlist）的条目只有 url，没有 webpage_url；
            // 插件用 url_result 返回的条目也常是 _type=url。两种都要认。
            episode.url = stringOf(entry.value(QStringLiteral("webpage_url")));
            if (episode.url.isEmpty())
                episode.url = stringOf(entry.value(QStringLiteral("url")));
            if (episode.url.isEmpty())
                episode.url = stringOf(entry.value(QStringLiteral("original_url")));
            if (!episode.url.isEmpty()) {
                const QUrl parsedUrl(episode.url);
                if (parsedUrl.isRelative() && !info.url.isEmpty())
                    episode.url = QUrl(info.url).resolved(parsedUrl).toString();
            }
            episode.durationMs =
                static_cast<qint64>(durationSecondsOf(entry) * 1000);
            episode.number = intOf(entry.value(QStringLiteral("episode_number")));

            // 没有播放地址的条目（下架/占位）不算一集，列出来也无法点击
            if (episode.url.isEmpty()) {
                ++dropped;
                continue;
            }
            info.episodes.append(episode);
        }

        LOG_DEBUG("Format") << "识别为多集: 条目=" << entries.size()
                            << " 有效=" << info.episodes.size()
                            << " 无地址=" << dropped
                            << " 标题=" << info.playlistTitle;
    }

    // 裸 m3u8 链接的直播兜底：yt-dlp 可能既不标 is_live 也不给 duration。
    // 只认源地址本身是 HLS 清单的情况；点播 HLS 的 duration 也可能为空，
    // 不能仅凭 formats 里有 m3u8 就判成直播（插件站点常见）。
    if (!info.live && info.durationMs <= 0 && looksLikeHlsManifest(info.url))
        info.live = true;

    LOG_DEBUG("Format") << "解析媒体: formats=" << info.formats.size()
                        << " entries=" << entries.size()
                        << " episodes=" << info.episodes.size()
                        << " live=" << info.live
                        << " durationMs=" << info.durationMs;
    return info;
}

// ============================
// 剧集：itemId 与标题的生成规则
// ============================

namespace Episodes {

QString itemId(const Episode &episode)
{
    if (!episode.id.isEmpty())
        return QStringLiteral("ep") + episode.id;
    // 没有平台编号的源：用 URL 的哈希兜底（跨客户端仍然确定 —— 同一集 URL 相同）。
    // 哈希而不是原样带上 url:itemId 要进协议，别塞一长串带签名的地址
    const QByteArray digest = QCryptographicHash::hash(episode.url.toUtf8(),
                                                       QCryptographicHash::Sha1);
    return QStringLiteral("url") + QString::fromLatin1(digest.toHex().left(16));
}

QString displayTitle(const Episode &episode, int fallbackIndex)
{
    const int number = episode.number > 0 ? episode.number : fallbackIndex;
    // 平台的标题常常自带集号（"1 闲聊的学生会"）—— 那就别再叠一层"第 1 集"
    const QString trimmed = episode.title.trimmed();
    if (!trimmed.isEmpty()
        && trimmed.startsWith(QString::number(number), Qt::CaseInsensitive)) {
        return trimmed;
    }
    // 站点标题常已带集号（"… 第1集"），别再叠一层"第 1 集 ·"
    if (!trimmed.isEmpty()) {
        const QRegularExpression embedded(
            QStringLiteral("第\\s*%1\\s*[集话話]").arg(number));
        if (embedded.match(trimmed).hasMatch())
            return trimmed;
    }
    if (!trimmed.isEmpty())
        return QStringLiteral("第 %1 集 · %2").arg(number).arg(trimmed);
    return QStringLiteral("第 %1 集").arg(number);
}

} // namespace Episodes

// ============================
// 简单视图
// ============================

QList<QualityOption> qualityOptions(const MediaInfo &info)
{
    QList<QualityOption> options;

    // ---- 直播：HLS 的 variant 就是"清晰度档" ----
    // **不能按 height 归纳**(直播的流没有 width/height),按 yt-dlp 的 quality
    // 序号分组，每档只留一条代表（见 liveTiers）。返回的 spec 就是 m3u8 地址 ——
    // 直播没有"下载",选哪档就是拿哪条流去拉。
    if (info.live) {
        for (const QPair<int, MediaFormat> &group : liveTiers(info)) {
            const MediaFormat &format = group.second;
            QualityOption option;
            // 记 format_id 而不是带签名的地址：`expires=` 那串会过期，
            // 而条目可能在房间里挂很久 —— 播放时拿 id 重新解析，地址是新鲜的
            option.spec = format.formatId;
            option.label = !format.formatNote.isEmpty() ? format.formatNote
                           : format.height > 0 ? tierLabel(format.shortSide(), format.fps)
                                               : QStringLiteral("默认");
            QStringList detail;
            if (!format.videoCodecName().isEmpty())
                detail << format.videoCodecName();
            if (format.isHdr())
                detail << QStringLiteral("HDR");
            if (detail.isEmpty())
                detail << format.protocol;
            option.detail = detail.join(QStringLiteral(" · "));
            option.height = format.height;
            option.hasHdr = format.isHdr();
            options.append(option);
        }
        return options;
    }

    // ---- 普通视频：按画面高度归纳 ----
    // 使用 formats 的 height 字段（视频像素高），档位 spec 记录的也是它——
    // yt-dlp 的 `-S res:N` 以此为比较依据。竖屏视频（1080x1920）的 height 为 1920，
    // 故「1080P 档」的 spec 是 "1920"。**仅显示用的 label 取短边**（shortSide），
    // 因为用户所说的 1080P 指短边。
    QList<int> heights;
    for (const MediaFormat &format : info.formats) {
        if (!format.isVideo() || format.height <= 0)
            continue;
        if (!heights.contains(format.height))
            heights.append(format.height);
    }
    std::sort(heights.begin(), heights.end(), std::greater<int>());

    for (const int height : heights) {
        QList<MediaFormat> tier;
        for (const MediaFormat &format : info.formats) {
            if (format.isVideo() && format.height == height)
                tier.append(format);
        }
        // 有 SDR 就展示 SDR 的（用户选"允许 HDR"是另一码事，档位本身不变）
        MediaFormat best = bestOf(tier, /*allowHdr=*/false);
        const bool onlyHdr = !best.valid;
        if (onlyHdr)
            best = bestOf(tier, /*allowHdr=*/true);

        QualityOption option;
        // spec 用 height 字段的数字 —— 它是 yt-dlp `-S res:N` 的比较对象，
        // 必须和 formats 里的值一致（竖屏视频也在 1920/1280 这一列）
        option.spec = QString::number(height);
        // 显示用短边 —— 竖屏 1080x1920 要显示成 "1080P" 而不是 "1920P"
        option.label = tierLabel(best.shortSide(), best.fps);
        option.height = height;
        option.hasHdr = onlyHdr;

        QStringList detail;
        if (!best.videoCodecName().isEmpty())
            detail << best.videoCodecName();
        const QString bitrate = bitrateText(best.tbr > 0 ? best.tbr : best.vbr);
        if (!bitrate.isEmpty())
            detail << bitrate;
        if (onlyHdr)
            detail << QStringLiteral("仅 HDR");
        option.detail = detail.join(QStringLiteral(" · "));

        options.append(option);
    }

    return options;
}

// ============================
// 详情视图
// ============================

QList<FormatChoice> detailChoices(const MediaInfo &info)
{
    QList<FormatChoice> choices;

    // ---- 直播：只列**能直接拉的** HLS,并按清晰度去重 ----
    // 一个清晰度在 formats 里往往有 4 条（m3u8/flv × avc/hevc）,全列出来是噪声；
    // 而且 FLV 那几条 Qt 根本拉不动，列了也不能选。这里只留每档的 HLS 代表。
    if (info.live) {
        QList<MediaFormat> playable;
        for (const MediaFormat &format : info.formats) {
            // HLS 和 HTTP-FLV 都可以直接交给播放器；斗鱼只提供 FLV 直链
            if (!format.isDirectPlayable())
                continue;
            if (format.url.isEmpty() && format.manifestUrl.isEmpty())
                continue;
            playable.append(format);
        }
        std::sort(playable.begin(), playable.end(),
                  [](const MediaFormat &a, const MediaFormat &b) {
                      return liveBetter(a, b);
                  });

        QList<int> seenRanks;
        for (const MediaFormat &format : playable) {
            const int rank = liveRank(format);
            if (seenRanks.contains(rank))
                continue;
            seenRanks.append(rank);

            FormatChoice choice;
            choice.formatId = format.formatId; // 同 qualityOptions:记 id,地址会过期
            choice.role = 0;
            choice.hdr = format.isHdr();
            choice.title = format.formatNote.isEmpty() ? QStringLiteral("直播流")
                                                       : format.formatNote;
            if (!format.videoCodecName().isEmpty())
                choice.title += QStringLiteral(" · ") + format.videoCodecName();
            if (format.isHdr())
                choice.title += QStringLiteral(" · HDR");
            choice.detail = format.protocol;
            choices.append(choice);
        }
        return choices;
    }


    auto add = [&choices](const MediaFormat &format, int role) {
        FormatChoice choice;
        choice.formatId = format.formatId;
        choice.role = role;
        choice.hdr = format.isHdr();

        QStringList title;
        if (role == 1) {
            title << QStringLiteral("音频");
            if (!format.audioCodecName().isEmpty())
                title << format.audioCodecName();
            const QString channels = channelsName(format.audioChannels);
            if (!channels.isEmpty())
                title << channels;
        } else {
            // 直播的流没有 width/height,分辨率用 format_note(原画/蓝光/超清)
            const QString resolution = resolutionText(format);
            if (!resolution.isEmpty())
                title << resolution;
            else if (!format.formatNote.isEmpty())
                title << format.formatNote;
            else
                title << QStringLiteral("未知画面");
            if (!format.videoCodecName().isEmpty())
                title << format.videoCodecName();
            if (format.isHdr())
                title << QStringLiteral("HDR");
            if (role == 2 && !format.audioCodecName().isEmpty())
                title << QStringLiteral("+ %1").arg(format.audioCodecName());
        }
        choice.title = title.join(QStringLiteral(" · "));

        QStringList detail;
        const QString bitrate = bitrateText(role == 1 ? format.abr : format.tbr);
        if (!bitrate.isEmpty())
            detail << bitrate;
        const QString size = sizeText(format.bestSize());
        if (!size.isEmpty())
            detail << QStringLiteral("约 %1").arg(size);
        if (!format.ext.isEmpty())
            detail << format.ext;
        if (role != 1 && !format.protocol.isEmpty()
            && !format.protocol.startsWith(QLatin1String("https"))) {
            detail << format.protocol;
        }
        choice.detail = detail.join(QStringLiteral(" · "));
        choices.append(choice);
    };

    // 视频 / 音频 / 合一分三组，组内都按优先级排
    auto sortedKind = [&info](MediaFormat::Kind kind) {
        QList<MediaFormat> list = filterKind(info.formats, kind);
        std::sort(list.begin(), list.end(),
                  [](const MediaFormat &a, const MediaFormat &b) {
                      return preferenceBetter(a, b);
                  });
        return list;
    };

    for (const MediaFormat &format : sortedKind(MediaFormat::VideoOnly))
        add(format, 0);
    for (const MediaFormat &format : sortedKind(MediaFormat::AudioOnly))
        add(format, 1);
    for (const MediaFormat &format : sortedKind(MediaFormat::Combined))
        add(format, 2);

    return choices;
}

// ============================
// 按 spec 解析
// ============================

MediaSelection resolve(const QString &spec, const MediaInfo &info,
                       bool allowHdrOverride)
{
    MediaSelection selection;
    selection.live = info.live;

    // spec 的 `:h` 表示“这个条目允许 HDR”；本地设置只能再收紧。
    const bool allowHdr = StreamSpec::hdrAllowed(spec) && allowHdrOverride;
    const QString body = StreamSpec::body(spec);

    LOG_DEBUG("Format") << "解析 spec: spec=" << (spec.isEmpty() ? "(空)" : spec)
                        << " live=" << (info.live ? 1 : 0)
                        << " HDR允许=" << (allowHdr ? 1 : 0)
                        << " 流数=" << info.formats.size();

    // ---- 直播：spec 是 m3u8 的地址，或某条流的 format_id ----
    if (info.live) {
        if (body.startsWith(QLatin1String("http"))) {
            selection.valid = true;
            selection.direct = true;
            selection.url = body;
            selection.label = QStringLiteral("直播源");
            return selection;
        }

        // 手选了具体某条流（详情视图里的 format_id）:用它的地址
        if (!body.isEmpty()) {
            for (const MediaFormat &format : info.formats) {
                if (format.formatId != body)
                    continue;
                const QString target = format.manifestUrl.isEmpty() ? format.url
                                                                    : format.manifestUrl;
                if (target.isEmpty())
                    break;
                selection.valid = true;
                selection.direct = true;
                selection.url = target;
                selection.label = format.formatNote.isEmpty() ? QStringLiteral("直播源")
                                                              : format.formatNote;
                return selection;
            }
        }

        // 没给 spec(或认不出来):按清晰度优先级挑一条能拉的 HLS(规则见 pickLiveHls)
        const MediaFormat chosen = pickLiveHls(info);
        const QString target = chosen.manifestUrl.isEmpty() ? chosen.url : chosen.manifestUrl;
        if (target.isEmpty())
            return selection; // 没有可拉取的流，交由上层报「该直播源无法播放」

        selection.valid = true;
        selection.direct = true;
        selection.url = target;
        selection.label = chosen.formatNote.isEmpty() ? QStringLiteral("直播源")
                                                      : chosen.formatNote;
        return selection;
    }

    // ---- 普通视频 ----

    // 1) 显式给了直链（手选的 url）:直接交给播放器
    if (body.startsWith(QLatin1String("http"))) {
        selection.valid = true;
        selection.direct = true;
        selection.url = body;
        selection.label = QStringLiteral("直链");
        return selection;
    }

    // 2) 手选的 format_id：新条目用 "id:30102+30280" / "id:30080" 显式标记；
    //    旧条目保留裸 id 兼容。裸数字既可能是档位（1080）也可能是 format_id
    //    （30080/137），判定顺序：id: 前缀 → 有 + → 该数字确实是源里的高度 → 精确 id。
    const bool prefixedIds = body.startsWith(QLatin1String("id:"));
    const QString idBody = prefixedIds ? body.mid(3) : body;
    const int specHeight = StreamSpec::heightOf(spec);
    const bool heightExists =
        specHeight > 0
        && std::any_of(info.formats.cbegin(), info.formats.cend(),
                       [specHeight](const MediaFormat &format) {
                           return format.isVideo() && format.height == specHeight;
                       });
    const bool idExists =
        !idBody.isEmpty()
        && std::any_of(info.formats.cbegin(), info.formats.cend(),
                       [&idBody](const MediaFormat &format) {
                           return format.formatId == idBody;
                       });

    const bool looksLikeIds = prefixedIds
                              || idBody.contains(QLatin1Char('+'))
                              || (!idBody.isEmpty() && specHeight < 0)
                              || (!heightExists && idExists);
    if (looksLikeIds) {
        const QStringList ids = idBody.split(QLatin1Char('+'));
        MediaFormat video;
        MediaFormat audio;
        for (const MediaFormat &format : info.formats) {
            if (!ids.contains(format.formatId))
                continue;
            if (format.isVideo() && !video.valid)
                video = format;
            else if (format.isAudio() && !format.isVideo() && !audio.valid)
                audio = format;
        }

        // 一个 id 都不匹配（对方登录态/会员等级不同）。
        // 显式 id: 是用户明确的选择，失败就报错，不静默换成最高画质；
        // 旧条目的裸 id 保留“按档位重挑”的兼容兜底。
        if (!video.valid && !audio.valid) {
            if (prefixedIds) {
                selection.error = QStringLiteral(
                    "手选的流（id:%1）在当前视频源中不存在；"
                    "可能是登录态/会员等级不同，或该流已下架")
                                       .arg(idBody);
                LOG_WARN("Format") << selection.error << "spec=" << spec;
                return selection;
            }
            LOG_WARN("Format") << "手选的 format_id 均不在源中，退回按档位挑选: spec="
                               << spec;
            return tierSelection(info, 0, allowHdr);
        }

        if (!video.valid) {
            // 仅提供了音频时，画面补优先级最高的一条
            video = bestOf(filterKind(info.formats, MediaFormat::VideoOnly), allowHdr);
        }

        // 手选的流也必须遵守 spec 的 HDR 许可（以及本地的更严格设置）。
        // 没有 `:h` 的手选 HDR 流一律拒绝，不再为旧 spec 留兼容路径。
        if (!allowHdr && video.isHdr()) {
            selection.error = QStringLiteral(
                "手选的流是 HDR，但当前条目/设置不允许 HDR；"
                "请选择 SDR 流，或允许 HDR 后重新添加");
            LOG_WARN("Format") << selection.error << "spec=" << spec;
            return selection;
        }

        if (video.isCombined()) {
            // 合一格式：无需音频，且能直链播放
            selection.valid = true;
            selection.direct = true;
            selection.video = video;
            selection.audio = video;
            selection.url = video.url.isEmpty() ? video.manifestUrl : video.url;
            // 直链试拉失败时还能退回 -f <id> 下载
            selection.formatExpression = video.formatId;
            selection.label = video.displayName();
            return selection;
        }

        if (!video.valid) {
            selection.error = QStringLiteral("手选的条目里没有可用的画面流");
            LOG_WARN("Format") << selection.error << "spec=" << spec;
            return selection;
        }

        if (!audio.valid)
            audio = pickBestAudio(info, allowHdr);

        selection.valid = true;
        selection.video = video;
        selection.audio = audio;
        selection.label = video.displayName();
        // 精确选择即精确选择：缺少任一路由 yt-dlp 报错，不静默替换为其他流。
        // 但如果某一路没有 format_id，不能拼出 "+audio" 或 "video+" 这种
        // 只选一半的表达式；清空后交给下载器按 -S/默认规则处理。
        if (video.formatId.isEmpty()
            || (audio.valid && audio.formatId.isEmpty())) {
            selection.formatExpression.clear();
        } else {
            selection.formatExpression = audio.valid
                ? QStringLiteral("%1+%2").arg(video.formatId, audio.formatId)
                : video.formatId;
        }
        return selection;
    }

    // 3) 分辨率档（"1080" → 不超过 1080 且尽量接近）或空串（不设限）
    return tierSelection(info, StreamSpec::heightOf(spec), allowHdr);
}

} // namespace MediaFormats
