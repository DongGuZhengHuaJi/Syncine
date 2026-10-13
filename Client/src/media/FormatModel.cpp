#include "FormatModel.h"

#include <algorithm>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QCryptographicHash>

namespace {

// ---- 数值兜底 ----
// yt-dlp 的 JSON 里这些字段可能是 null(拿不到),toDouble/toInteger 会当 0 ——
// 但 filesize 为 0 和"没有 filesize"在界面上要区分,所以统一收进一个字段再判

double doubleOf(const QJsonValue &value)
{
    // null / 缺字段都会到这里;toDouble 对 null 返回 0
    return value.isDouble() ? value.toDouble() : 0;
}

qint64 int64Of(const QJsonValue &value)
{
    if (!value.isDouble())
        return 0;
    return static_cast<qint64>(value.toDouble());
}

// vcodec/acodec 在 yt-dlp 的 JSON 里是**字符串**:"none" 表示这一路没有。
// 但也有站点给 null(少见),同样按 none 算
bool hasCodec(const QJsonValue &value)
{
    const QString text = value.toString();
    return !text.isEmpty() && text != QLatin1String("none");
}

// ---- 编码家族 ----
// 把 vcodec/acodec 的完整串("avc1.640032" / "hvc1.1.6.L150.90" / "mp4a.40.2")
// 归纳成用户能认的名字。分不清就原样返回,别硬编。
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

// 分辨率档的标签。60 这种整数帧率标出来,其余不标(24/25/30 标出来是噪声)
QString tierLabel(int height, double fps)
{
    QString label = QStringLiteral("%1P").arg(height);
    if (fps >= 50)
        label += QStringLiteral("60");
    return label;
}

// ---- 动态范围 ----
// yt-dlp 的 dynamic_range 对 SDR 视频通常是 "SDR",HDR 是 "HDR"/"HLG",
// 杜比视界是 "DV"。但**有的站点干脆给 null** —— 那就当 SDR,别误报。
bool dynamicRangeIsHdr(const QString &range)
{
    if (range.isEmpty() || range.compare(QLatin1String("SDR"), Qt::CaseInsensitive) == 0)
        return false;
    const QString text = range.toLower();
    return text.contains(QLatin1String("hdr")) || text.contains(QLatin1String("hlg"))
           || text == QLatin1String("dv") || text.contains(QLatin1String("dolby"))
           || text.contains(QLatin1String("dovi"));
}

// 编码家族优先级:同分辨率下 h264 优先。
// 理由:① 兼容性最好(Qt 和未来可能的推流都吃)② B 站的 avc 档常常就是高码率档
// (实测 1080P 的 30112 avc ~3Mbps vs 30102 hevc ~1.5Mbps),码率高 → 画质好。
// 想改偏好只改这张表的顺序。
int vcodecRank(const QString &vcodec)
{
    const QString name = videoCodecName(vcodec);
    if (name == QLatin1String("H.264")) return 0;
    if (name == QLatin1String("HEVC"))  return 1;
    if (name == QLatin1String("AV1"))   return 2;
    if (name == QLatin1String("VP9"))   return 3;
    return 4;
}

// 音频:同档位下 AAC 优先(兼容性),其次 Opus/Vorbis
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
    // 纯画面 优于 合一:合一格式不含高清(高清要分流),而且精确拿两路更可控
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

// 按"简单模式的优先级"比较两条流:分辨率 → 帧率 → 编码 → 种类。
// 这是 `-S` 排序规则在本地的一份镜像(弹窗展示、手选兜底都用它)。
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
    return a.formatId < b.formatId; // 兜底:让结果稳定,别人重跑也一样
}

// 在 candidates 里挑优先级最高的;allowHdr=false 时只从非 HDR 里挑
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

// 按分辨率档挑画面:height = formats 里的 height 字段(HDR 视频在 B 站是
// 另一条 hevc 流,`dynamic_range:sdr` 会把它筛掉,挑不到再放开)
MediaFormat pickVideoTier(const MediaInfo &info, int height, bool allowHdr)
{
    QList<MediaFormat> tier;
    for (const MediaFormat &format : info.formats) {
        if (!format.isVideo() || format.height <= 0)
            continue;
        if (height > 0 && format.height != height)
            continue;
        tier.append(format);
    }
    MediaFormat best = bestOf(tier, allowHdr);
    if (best.formatId.isEmpty() && !allowHdr)
        best = bestOf(tier, true);
    return best;
}

MediaFormat pickBestAudio(const MediaInfo &info, bool allowHdr)
{
    return bestOf(filterKind(info.formats, MediaFormat::AudioOnly), allowHdr);
}

// 直播看的"清晰度"只能靠 yt-dlp 的 quality 序号(实测 B 站 原画=4 > 蓝光=3 > 超清=2)。
// 有的站点会给 height,那就更准,优先用它。
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
    // 同一清晰度有多条(不同协议/编码):
    //   m3u8 优先 —— Qt 的播放器能直接拉 HLS;FLV/DASH 不行
    const bool hlsA = a.protocol.contains(QLatin1String("m3u8"));
    const bool hlsB = b.protocol.contains(QLatin1String("m3u8"));
    if (hlsA != hlsB)
        return hlsA;
    // 编码:avc 优先。实测 B 站直播的 HEVC variant 常常要 Referer,而 h264 那条不用
    const int codecA = vcodecRank(a.vcodec);
    const int codecB = vcodecRank(b.vcodec);
    if (codecA != codecB)
        return codecA < codecB;
    return a.formatId < b.formatId;
}

// 按清晰度分组,挑出每组代表(见 liveBetter);组号用来去重、组的代表决定顺序
QList<QPair<int, MediaFormat>> liveTiers(const MediaInfo &info)
{
    QList<QPair<int, MediaFormat>> groups;
    for (const MediaFormat &format : info.formats) {
        // 只考虑"能直接拉"的(http(s) / HLS):FLV、DASH 那种播放器吃不下
        if (!format.isDirectPlayable())
            continue;
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
                  // 清晰度高的在前;一样就按协议/编码优先级
                  if (a.first != b.first)
                      return a.first > b.first;
                  return liveBetter(a.second, b.second);
              });
    return groups;
}

// 从直播 formats 里挑一条能拉的 HLS:清晰度最高的那一档里,
// 优先 avc(实测 B 站直播 HEVC 那条要 Referer,h264 的不用);
// 地址优先 manifest_url(master 清单能带 EXT-X-MEDIA、播放器自适应),
// 它为空时用格式自己的 url —— 实测 B 站直播是后者。
MediaFormat pickLiveHls(const MediaInfo &info)
{
    const QList<QPair<int, MediaFormat>> groups = liveTiers(info);
    MediaFormat fallback; // 没有 m3u8 时退而求其次(FLV 也拉不动,只是让上层有东西可报)
    for (const QPair<int, MediaFormat> &group : groups) {
        const MediaFormat &format = group.second;
        if (!format.protocol.contains(QLatin1String("m3u8")))
            continue;
        if (vcodecRank(format.vcodec) == 0) // 已经是 avc,最优先
            return format;
        if (fallback.formatId.isEmpty())
            fallback = format;
    }
    if (!fallback.formatId.isEmpty())
        return fallback;
    return groups.isEmpty() ? MediaFormat() : groups.first().second;
}

// 按档位下:`-f` 交给 yt-dlp 的 `bv*+ba/b` 自己挑,画质由 `-S` 的排序说了算。
// **不要把档位解析成具体 format_id 钉死** ——那样一旦对方 formats 变了(登录态不同),
// 精确 id 下不到就白失败;`-S` 是"给我们这一档优先级最高的一条",永远有解。
MediaSelection tierSelection(const MediaInfo &info, int height, bool allowHdr)
{
    const MediaFormat video = pickVideoTier(info, height, allowHdr);

    MediaSelection selection;
    selection.live = false;
    selection.hdr = video.isHdr();
    selection.video = video;
    selection.label = video.displayName();

    // 这一档整档都是 HDR 的:Qt 播放要过一遍 HdrConverter,这里提前标出来
    if (video.formatId.isEmpty()) {
        // 一档都没挑出来(源里没这个高度):退回不设限
        return tierSelection(info, 0, allowHdr);
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
    else if (text.contains(QLatin1Char('+'))) {
        // 手选的 format_id 对:尽量还原成"什么分辨率",还原不了就只写"手选"
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
        // 档位里存的是 height 字段(竖屏 1080P 存的是 "1920")。要显示成
        // "1080P" 得知道这条流的长宽比 —— 在 formats 里找同高的一条问短边;
        // 列表那边手上没有 formats(传的是空 info),只能原样显示成 "1920P"
        const int height = text.toInt();
        int labelHeight = height;
        for (const MediaFormat &format : info.formats) {
            if (format.height == height && format.isVideo()) {
                labelHeight = format.shortSide();
                break;
            }
        }
        base = tierLabel(labelHeight, 0);
    } else {
        base = text; // 认不出来就原样显示,总比空着强
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
    for (const QJsonValue &value : formats) {
        const QJsonObject object = value.toObject();

        const bool hasVideo = hasCodec(object.value(QStringLiteral("vcodec")));
        const bool hasAudio = hasCodec(object.value(QStringLiteral("acodec")));
        // 两个都没有 = 故事板/缩略图,不是媒体;有 vcodec 也可能来自 url 为空的项
        if (!hasVideo && !hasAudio)
            continue;

        MediaFormat format;
        format.formatId = object.value(QStringLiteral("format_id")).toString();
        format.url = object.value(QStringLiteral("url")).toString();
        format.protocol = object.value(QStringLiteral("protocol")).toString();
        format.manifestUrl = object.value(QStringLiteral("manifest_url")).toString();
        format.width = object.value(QStringLiteral("width")).toInt(0);
        format.height = object.value(QStringLiteral("height")).toInt(0);
        format.fps = doubleOf(object.value(QStringLiteral("fps")));
        format.vcodec = object.value(QStringLiteral("vcodec")).toString();
        format.acodec = object.value(QStringLiteral("acodec")).toString();
        format.vbr = doubleOf(object.value(QStringLiteral("vbr")));
        format.abr = doubleOf(object.value(QStringLiteral("abr")));
        format.tbr = doubleOf(object.value(QStringLiteral("tbr")));
        format.filesize = int64Of(object.value(QStringLiteral("filesize")));
        format.filesizeApprox = int64Of(object.value(QStringLiteral("filesize_approx")));
        format.audioChannels = object.value(QStringLiteral("audio_channels")).toInt(0);
        format.language = object.value(QStringLiteral("language")).toString();
        format.dynamicRange = object.value(QStringLiteral("dynamic_range")).toString();
        format.formatNote = object.value(QStringLiteral("format_note")).toString();
        format.quality = object.value(QStringLiteral("quality")).toInt(0);
        format.ext = object.value(QStringLiteral("ext")).toString();

        if (hasVideo && hasAudio)
            format.kind = MediaFormat::Combined;
        else if (hasVideo)
            format.kind = MediaFormat::VideoOnly;
        else
            format.kind = MediaFormat::AudioOnly;

        result.append(format);
    }
    return result;
}

MediaInfo parseMediaInfo(const QJsonObject &root)
{
    MediaInfo info;
    info.title = root.value(QStringLiteral("title")).toString();
    if (info.title.isEmpty())
        info.title = root.value(QStringLiteral("fulltitle")).toString();
    info.uploader = root.value(QStringLiteral("uploader")).toString();
    if (info.uploader.isEmpty())
        info.uploader = root.value(QStringLiteral("channel")).toString();
    info.url = root.value(QStringLiteral("webpage_url")).toString();

    const double seconds = doubleOf(root.value(QStringLiteral("duration")));
    info.durationMs = static_cast<qint64>(seconds * 1000);

    info.live = root.value(QStringLiteral("is_live")).toBool(false);
    info.wasLive = root.value(QStringLiteral("was_live")).toBool(false);

    info.formats = parse(root.value(QStringLiteral("formats")).toArray());

    // ---- 多集(剧集/合集):顶层没有 formats,每一集自带 ----
    // yt-dlp 的 _type 是 "playlist";判据是 **顶层 formats 为空 + entries 非空**。
    // 不用 entries.length 单独判:单视频理论上也可能有 entries,而"顶层 formats
    // 为空"能把那种情况排除掉
    const QJsonArray entries = root.value(QStringLiteral("entries")).toArray();
    if (info.formats.isEmpty() && !entries.isEmpty()) {
        info.playlistTitle = info.title;
        info.title.clear();   // 顶层 title 是"整季"的名字,不是某一条的名字
        int index = 0;
        for (const QJsonValue &value : entries) {
            const QJsonObject entry = value.toObject();
            ++index;

            Episode episode;
            episode.id = entry.value(QStringLiteral("id")).toString();
            episode.title = entry.value(QStringLiteral("title")).toString();
            episode.url = entry.value(QStringLiteral("webpage_url")).toString();
            const double seconds = doubleOf(entry.value(QStringLiteral("duration")));
            episode.durationMs = static_cast<qint64>(seconds * 1000);
            episode.number = entry.value(QStringLiteral("episode_number")).toInt(0);

            // 连播放地址都没有的条目(下架/占位)不算一集,列出来也点不动
            if (episode.url.isEmpty())
                continue;
            info.episodes.append(episode);
        }
    }

    // 如果 yt-dlp 没给 is_live,但 duration=0,且 formats 里有 m3u8,认为是直播
    if (!info.live && info.durationMs <= 0) {
        for (const MediaFormat &format : info.formats) {
            if (format.protocol.contains(QLatin1String("m3u8"))) {
                info.live = true;
                break;
            }
        }
    }
    return info;
}

// ============================
// 剧集:itemId 与标题的生成规则
// ============================

namespace Episodes {

QString itemId(const Episode &episode)
{
    if (!episode.id.isEmpty())
        return QStringLiteral("ep") + episode.id;
    // 没有平台编号的源:用 URL 的哈希兜底(跨客户端仍然确定 —— 同一集 URL 相同)。
    // 哈希而不是原样带上 url:itemId 要进协议,别塞一长串带签名的地址
    const QByteArray digest = QCryptographicHash::hash(episode.url.toUtf8(),
                                                       QCryptographicHash::Sha1);
    return QStringLiteral("url") + QString::fromLatin1(digest.toHex().left(16));
}

QString displayTitle(const Episode &episode, int fallbackIndex)
{
    const int number = episode.number > 0 ? episode.number : fallbackIndex;
    // 平台的标题常常自带集号("1 闲聊的学生会")—— 那就别再叠一层"第 1 集"
    const QString trimmed = episode.title.trimmed();
    if (!trimmed.isEmpty()
        && trimmed.startsWith(QString::number(number), Qt::CaseInsensitive)) {
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

    // ---- 直播:HLS 的 variant 就是"清晰度档" ----
    // **不能按 height 归纳**(直播的流没有 width/height),按 yt-dlp 的 quality
    // 序号分组,每档只留一条代表(见 liveTiers)。返回的 spec 就是 m3u8 地址 ——
    // 直播没有"下载",选哪档就是拿哪条流去拉。
    if (info.live) {
        for (const QPair<int, MediaFormat> &group : liveTiers(info)) {
            const MediaFormat &format = group.second;
            QualityOption option;
            // 记 format_id 而不是带签名的地址:`expires=` 那串会过期,
            // 而条目可能在房间里挂很久 —— 播放时拿 id 重新解析,地址是新鲜的
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

    // ---- 普通视频:按画面高度归纳 ----
    //
    // 注意这里用的是 formats 的 **height 字段**(视频像素高),档位 spec 记的
    // 也是它 —— yt-dlp 的 `-S res:N` 拿它做比较。竖屏视频(1080x1920)的
    // height 是 1920,所以"1080P 这一档"的 spec 是 "1920",选 720P 是 "1280"。
    // **只有显示给人看的 label 用短边**(shortSide):用户说的 1080P 是短边。
    // 播放时不再按 height 挑流,选中的档位由 `-S res:<height>` 交给 yt-dlp,
    // 所以这里的数字和 formats 保持一致就不会错位。
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
        // 有 SDR 就展示 SDR 的(用户选"允许 HDR"是另一码事,档位本身不变)
        MediaFormat best = bestOf(tier, /*allowHdr=*/false);
        const bool onlyHdr = best.formatId.isEmpty();
        if (onlyHdr)
            best = bestOf(tier, /*allowHdr=*/true);

        QualityOption option;
        // spec 用 height 字段的数字 —— 它是 yt-dlp `-S res:N` 的比较对象,
        // 必须和 formats 里的值一致(竖屏视频也在 1920/1280 这一列)
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

    // ---- 直播:只列**能直接拉的** HLS,并按清晰度去重 ----
    // 一个清晰度在 formats 里往往有 4 条(m3u8/flv × avc/hevc),全列出来是噪声;
    // 而且 FLV 那几条 Qt 根本拉不动,列了也不能选。这里只留每档的 HLS 代表。
    if (info.live) {
        QList<MediaFormat> playable;
        for (const MediaFormat &format : info.formats) {
            if (!format.protocol.contains(QLatin1String("m3u8")))
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

    // 视频 / 音频 / 合一分三组,组内都按优先级排
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

MediaSelection resolve(const QString &spec, const MediaInfo &info)
{
    MediaSelection selection;
    selection.live = info.live;

    const bool allowHdr = StreamSpec::hdrAllowed(spec);
    const QString body = StreamSpec::body(spec);

    // ---- 直播:spec 是 m3u8 的地址,或某条流的 format_id ----
    if (info.live) {
        if (body.startsWith(QLatin1String("http"))) {
            selection.valid = true;
            selection.direct = true;
            selection.url = body;
            selection.label = QStringLiteral("直播源");
            return selection;
        }

        // 手选了具体某条流(详情视图里的 format_id):用它的地址
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
                selection.hdr = format.isHdr();
                return selection;
            }
        }

        // 没给 spec(或认不出来):按清晰度优先级挑一条能拉的 HLS(规则见 pickLiveHls)
        const MediaFormat chosen = pickLiveHls(info);
        const QString target = chosen.manifestUrl.isEmpty() ? chosen.url : chosen.manifestUrl;
        if (target.isEmpty())
            return selection; // 没有能拉的流,交给上层报"这个直播源放不了"

        selection.valid = true;
        selection.direct = true;
        selection.url = target;
        selection.label = chosen.formatNote.isEmpty() ? QStringLiteral("直播源")
                                                      : chosen.formatNote;
        selection.hdr = chosen.isHdr();
        return selection;
    }

    // ---- 普通视频 ----

    // 1) 显式给了直链(手选的 url):直接交给播放器
    if (body.startsWith(QLatin1String("http"))) {
        selection.valid = true;
        selection.direct = true;
        selection.url = body;
        selection.label = QStringLiteral("直链");
        return selection;
    }

    // 2) 手选的 format_id 对:"30102+30280" / "30080"
    const bool looksLikeIds = body.contains(QLatin1Char('+'))
                              || (!body.isEmpty() && StreamSpec::heightOf(spec) < 0);
    if (looksLikeIds) {
        const QStringList ids = body.split(QLatin1Char('+'));
        MediaFormat video;
        MediaFormat audio;
        for (const MediaFormat &format : info.formats) {
            if (!ids.contains(format.formatId))
                continue;
            if (format.isVideo() && video.formatId.isEmpty())
                video = format;
            else if (format.isAudio() && !format.isVideo() && audio.formatId.isEmpty())
                audio = format;
        }

        // 一个 id 都不匹配(对方登录态/会员等级不同)→ 退回按档位挑,
        // 别让"这条下不到"变成"这条播不了"
        if (video.formatId.isEmpty() && audio.formatId.isEmpty())
            return tierSelection(info, 0, allowHdr);

        if (video.formatId.isEmpty()) {
            // 只给了音频:画面补优先级最高的一条
            video = bestOf(filterKind(info.formats, MediaFormat::VideoOnly), allowHdr);
        }

        if (video.isCombined()) {
            // 合一格式:不需要音频,而且能直链播放
            selection.valid = true;
            selection.direct = true;
            selection.video = video;
            selection.audio = video;
            selection.url = video.url;
            selection.hdr = video.isHdr();
            selection.label = video.displayName();
            return selection;
        }

        if (video.formatId.isEmpty())
            return selection; // 连画面都挑不出来,交给上层报失败

        if (audio.formatId.isEmpty())
            audio = pickBestAudio(info, allowHdr);

        selection.valid = true;
        selection.video = video;
        selection.audio = audio;
        selection.hdr = video.isHdr();
        selection.label = video.displayName();
        // 精确选择就是精确选择:缺哪一路就让 yt-dlp 报错,不要静默换成别的流
        selection.formatExpression = audio.formatId.isEmpty()
            ? video.formatId
            : QStringLiteral("%1+%2").arg(video.formatId, audio.formatId);
        return selection;
    }

    // 3) 分辨率档("1080" → 不超过 1080 尽量接近)或空串(不设限)
    return tierSelection(info, StreamSpec::heightOf(spec), allowHdr);
}

} // namespace MediaFormats
