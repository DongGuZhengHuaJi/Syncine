//
// 媒体格式模型：把 yt-dlp 返回的 `formats[]` 解析为可用类型，并提供
// 「按 spec 挑流 / 生成下载参数」的全部规则。这些是纯规则（给定 formats 与 spec
// 答案唯一、不涉及进程/网络/Qt 信号），故独立成层以便单独验证。
//
// 两条播放路径的判定依据均在此：直链流播要求选中项为音视频合一（isCombined），
// 将 url 直接交给播放器；下载合并则是选中分离的画面 + 声音，需下载后合并
// （见 UrlDownloader.h）。
//
// spec 记录条目「要下载哪些流」，三种形态（`:h` 后缀表示允许 HDR，见 StreamSpec）：
//   "1080"              分辨率档：不超过且尽量接近 1080
//   "id:30102+30280"    详情视图手选的精确流（format_id 对；新条目带 id: 前缀）
//   "30102+30280"       旧条目的裸 id 形式，仍兼容
//   "id:30080"          手选单条流
//   "1080:h"            分辨率档或手选流，且允许 HDR（由调用方拼上/去掉）
//   空串 = 不设限（最高画质）
// 裸数字优先按“源里真实存在的分辨率高度”解释，其次才按 format_id；纯数字
// format_id（30080/137）不会再被当成档位静默忽略。
//
// 画质档位会把选中的流钉成精确的 `-f` 表达式；`-S` 只在对方缺少该 format_id
// 时作为回退。HDR 是例外中的例外：`-S dynamic_range:sdr` 只是排序，不是过滤，
// 所以关闭 HDR 时靠“精确 id + 下载后 ffprobe 复核”保证不会落成 HDR。
// 见 MediaSelection::formatExpression 与 UrlDownloader 的 checking 阶段。
//

#ifndef SYNCINE_FORMATMODEL_H
#define SYNCINE_FORMATMODEL_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QJsonArray;
class QJsonObject;

// 一条流（formats[] 中的一个元素），仅保留有意义的字段。
//
// kind 由 vcodec/acodec 推出 —— 这是 yt-dlp 的约定：
//   两个都是 "none" = 故事板/缩略图，不是媒体，解析时直接丢掉
//   只有 vcodec     = 纯画面（DASH 的视频轨）
//   只有 acodec     = 纯声音
//   两个都有        = 音视频合一（单文件格式，能直接交给播放器）
//   **两个都是 null/缺失** = 未知（generic 直链常见，如 TikTok CDN 的 MP4）：
//   若 video_ext 表明是视频文件，按"合一"处理；不能当 none 丢掉。
struct MediaFormat {
    enum Kind {
        Unknown,
        VideoOnly,
        AudioOnly,
        Combined,
    };

    QString formatId;
    QString url;
    QString protocol;      // https / m3u8_native / http_dash_segments ...
    QString manifestUrl;   // HLS 的 master 清单(直播要用)

    Kind kind = Unknown;

    int width = 0;
    int height = 0;
    double fps = 0;
    QString vcodec;
    QString acodec;
    double vbr = 0;        // 视频码率(kbps);纯音频项是 0
    double abr = 0;        // 音频码率(kbps)
    double tbr = 0;        // 总码率
    qint64 filesize = 0;         // 确切大小(常为 0)
    qint64 filesizeApprox = 0;   // 估算大小
    int audioChannels = 0;       // 0 = 未知
    QString language;
    QString dynamicRange;        // "SDR" / "HDR" / "HDR10" / "DV" / 空
    QString formatNote;          // "1080P 高码率" 之类的补充说明
    int quality = 0;             // yt-dlp 的清晰度序号(直播用:原画 > 蓝光 > 超清)
    QString ext;

    bool isVideo() const { return kind == VideoOnly || kind == Combined; }
    bool isAudio() const { return kind == AudioOnly || kind == Combined; }
    bool isCombined() const { return kind == Combined; }
    // 直链这条路（QMediaPlayer 直接加载）只吃这几类：普通 http(s) 文件和 HLS。
    // FLV 直播流、DASH 等拉不了，只能走下载合并 —— 而直播又没法"下完"。
    bool isDirectPlayable() const
    {
        return protocol.startsWith(QLatin1String("http"))
               || protocol.contains(QLatin1String("m3u8"));
    }

    // 显示用的"多少 P":取短边。
    // **不能直接用 height**:竖屏视频（1080x1920）的 height 是 1920 ——
    // 用户说的"1080P"和网站标的都是短边，拿长边会显示成 1920P。
    int shortSide() const
    {
        if (width > 0 && height > 0)
            return width < height ? width : height;
        return height;
    }
    // HDR(含杜比视界)。设置里关闭 HDR 时，下载端会拒绝这类流（见 resolve 的
    // allowHdrOverride）；这是硬约束，不再做离线转码。
    bool isHdr() const;

    // 体积：优先确切大小，没有就退回估算值；都拿不到返回 0
    qint64 bestSize() const { return filesize > 0 ? filesize : filesizeApprox; }

    // 展示名："1080P60 · H.264 · 1.5 Mbps · HDR · 约 48.8 MB"
    // (纯音频是 "音频 · AAC · 151 kbps · 立体声 · 约 4.8 MB")
    // **拼装放在这里**,QML 只负责画 —— 免得两边各拼一套、慢慢跑偏
    QString displayName() const;

    // 简短的编码家族名："H.264" / "HEVC" / "AV1" / "AAC" / "Opus" ...
    QString videoCodecName() const;
    QString audioCodecName() const;
};

// 一集（剧集类链接的 entries[] 里的一项）。
//
// 剧集不是"一条更大的视频",而是**一季里的几条视频** —— 所以进房间播放列表时
// 它是**多个条目**(见 AddVideoDialog 的剧集选择),不是一条。
struct Episode {
    QString id;        // 平台给这一集的编号(如 B 站 bangumi 的 ep11511 里的 11511)
    QString title;
    QString url;       // 这一集的 webpage_url(单独解析/下载要用它)
    qint64 durationMs = 0;
    int number = 0;    // 第几集(拿不到就是 0)
};

// 一次探测的全貌（title 这些通用字段也收着，以后做站点特化时再往这里加）
struct MediaInfo {
    QString url;
    QString title;
    QString uploader;
    qint64 durationMs = 0;
    bool live = false;       // 正在直播(没有时长、不能下载合并)
    bool wasLive = false;    // 直播回放:有时长,当普通视频处理

    // yt-dlp 的站点标识与平台内 id。弹幕层据此判断是不是 B 站源、
    // 以及在没有 cid 时按 BV/av/ep 调 API 兜底。
    QString extractor;       // "BiliBili" / "BiliBiliBangumi" / ...
    QString id;              // "BV1xx411c7mD" / "267851"(番剧 ep 号) / ...

    QList<MediaFormat> formats;

    // 多集（yt-dlp 的 _type == "playlist"）:顶层没有 formats,每一集自带。
    // 两条判据：**顶层 formats 为空** + episodes 非空 —— 单视频永远没有 episodes
    QList<Episode> episodes;
    QString playlistTitle;   // 合集/番剧的名字("学生会的一己之见")

    bool isEmpty() const { return formats.isEmpty() && title.isEmpty(); }
    bool isPlaylist() const { return !episodes.isEmpty(); }
};

// 简单视图的一行（分辨率档）。label 为主文字，detail 为右侧小字。
struct QualityOption {
    // 选中时条目记录的 spec。
    // 直播：该 variant 的 m3u8 地址；普通视频：高度上限数字（"1080"，0 = 不设限）
    QString spec;
    QString label;     // "1080P60"
    QString detail;    // "H.264 · 1.5 Mbps"；直播时为协议名
    int height = 0;    // 0 = 最高画质（不设限）
    bool hasHdr = false; // 该档位仅有 HDR 可选（界面给出提示）
};

// 详情视图的一行：一条具体的流，点击即等同于手选该条
struct FormatChoice {
    QString formatId;
    QString title;     // "1080x1920 · 30fps · H.264 高码率"
    QString detail;    // "1.5 Mbps · 约 48.8 MB"
    int role = 0;      // 0=视频 1=音频 2=合一
    bool hdr = false;
};

// 按 spec 挑选的结果
struct MediaSelection {
    bool valid = false;
    bool live = false;          // 直播：不下载，直接拉流
    bool direct = false;        // 选中合一格式，可将 url 直接交给播放器
    QString url;                // direct 时的直链（直播时为 m3u8）
    QString formatExpression;   // yt-dlp 的 -f 表达式
    QStringList sortArgs;       // yt-dlp 的 -S 排序参数（合并在一起）
    QString label;              // "1080P60 · H.264"，供日志与界面使用
    QString error;              // valid=false 时的具体原因（给用户看）
    MediaFormat video;          // 选中的画面流（直播时为空）
    MediaFormat audio;          // 选中的声音流（合一格式时等于 video）
};

// 条目上 spec 的编解码与规则。该字符串是跨客户端的共同约定，见文件头。
namespace StreamSpec {

// spec 是否带 HDR 后缀（":h"）
bool hdrAllowed(const QString &spec);
// 去掉后缀，只留主体（"1080:h" → "1080"）
QString body(const QString &spec);
// 主体转数字。注意取的是 formats 的 height 字段，竖屏 1080P 那条为 1920。
// 非数字（手选 format_id / 空串）返回 -1；空串单独处理。
int heightOf(const QString &spec);
// 界面用的短标签："最高画质" / "1080P" / "手选 1080P"；hdr 时后缀 "+HDR"。
// 数字档位要显示为短边（1080P）需对照 formats 找回真实的流，故 info 传空时
// 只能原样显示（"1920P"）——列表侧即如此，播放页传完整 info 才准确。
QString label(const QString &spec, const MediaInfo &info);
// 将界面选中的档位/手选流拼成带 HDR 后缀的 spec
QString withHdr(const QString &specBody, bool allowHdr);

} // namespace StreamSpec

namespace MediaFormats {

// yt-dlp 的 formats[] → MediaFormat 列表（丢弃故事板等非媒体项）
QList<MediaFormat> parse(const QJsonArray &formats);

// 整份 `-J` 输出 → MediaInfo。title/uploader/duration/live 均在此收集
MediaInfo parseMediaInfo(const QJsonObject &root);

// ---- 简单视图 ----
// 按画面高度归纳为档位：同高度的多条（编码不同）合并为一行，代表取该档中
// 优先级最高的一条（优先级规则见 pick）。由高到低排序。
QList<QualityOption> qualityOptions(const MediaInfo &info);

// ---- 详情视图 ----
// 每条流一行，按 视频 → 音频 → 合一 分组，组内按优先级排序
QList<FormatChoice> detailChoices(const MediaInfo &info);

// ---- 剧集：itemId 与标题的生成规则 ----
// 这两条放在此处（而非对话框内），因为它们是跨客户端的约定：
// 同一个 ep id 在任何机器上都应转换为同一个 itemId，否则同一集在房间列表中
// 会被视为两条（服务端按 itemId 查重）。
namespace Episodes {

// 该集的房间条目 id："ep<平台编号>"；编号取不到时退回 URL 哈希兜底（仍确定）
QString itemId(const Episode &episode);
// 列表中显示的名字："第 N 集 · 标题"（标题已含集号时不再重复加前缀）
QString displayTitle(const Episode &episode, int fallbackIndex);

} // namespace Episodes

// ---- 播放/下载 ----
// 按 spec 解析为要下载的流。live 的 formats 为清单中的 variant：
// 空 spec 给 master 清单（播放器自适应），具体 id 给对应 variant。
//
// allowHdrOverride 是**本地设置的硬上限**：即使 spec 带 `:h`，传 false 时也
// 只选 SDR 流。反过来不成立——spec 不带 `:h` 时，传 true 也不会选 HDR
// （条目的 spec 决定"这条内容是什么"，本地设置只能收紧，不能放宽）。
MediaSelection resolve(const QString &spec, const MediaInfo &info,
                       bool allowHdrOverride = true);

} // namespace MediaFormats

#endif //SYNCINE_FORMATMODEL_H
