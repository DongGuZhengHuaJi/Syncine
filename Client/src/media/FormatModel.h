//
// 媒体格式模型 —— 把 yt-dlp 返回的 `formats[]` 细解析成能用的类型,
// 并提供"按 spec 挑流 / 生成下载参数"的全部规则。
//
// 为什么单独一层(而不是塞进 UrlDownloader):
//   这些是**纯规则**——给一份 formats 和一条 spec,答案唯一。没有进程、没有网络、
//   没有 Qt 信号。放这里可以独立演算、独立验证,下载器和探测弹窗都只是调用方。
//
// ── 两条路的判定依据都在这里 ──
//   · 直链流播:选中的那条**音视频合一**(isCombined)→ 直接把 url 交给播放器
//   · 下载合并:选中的是分离的 画面+声音 → 只能下完再合并(见 UrlDownloader.h)
//
// ── spec:条目上记的"要下哪些流" ──
//   字符串,三种形态(分隔符 `:h` 是"允许 HDR/杜比视界",见 StreamSpec):
//     "1080"            分辨率档:不超过且尽量接近 1080
//     "30102+30280"     详情视图手选的精确流(format_id 对)
//     "1080:h"          分辨率档,且**不排除** HDR —— 由调用方拼上/去掉
//   空串 = 不设限(最高画质)。
//
//   挑选规则来自实测定论(见 memory:syncine-ytdlp-format-selection):
//   **画质靠 `-S` 排序,不靠 `-f [height<=N]` 筛选** —— 后者只筛不排,
//   实测 `bv*[height<=1080]+ba` 会下到 480P。所以这里生成的是
//   `-S "res:1080,fps,vcodec:h264,..."`,而 `-f` 永远只有 `bv*+ba/b`。
//

#ifndef SYNCINE_FORMATMODEL_H
#define SYNCINE_FORMATMODEL_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QJsonArray;
class QJsonObject;

// 一条流(formats[] 里的一个元素),只留对我们有意义的字段。
//
// kind 由 vcodec/acodec 是否为 "none" 推出 —— 这是 yt-dlp 的约定:
//   两个都是 none  = 故事板/缩略图,不是媒体,解析时直接丢掉
//   只有 vcodec     = 纯画面(DASH 的视频轨)
//   只有 acodec     = 纯声音
//   两个都有        = 音视频合一(单文件格式,能直接交给播放器)
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
    // 直链这条路(QMediaPlayer 直接加载)只吃这几类:普通 http(s) 文件和 HLS。
    // FLV 直播流、DASH 等拉不了,只能走下载合并 —— 而直播又没法"下完"。
    bool isDirectPlayable() const
    {
        return protocol.startsWith(QLatin1String("http"))
               || protocol.contains(QLatin1String("m3u8"));
    }

    // 显示用的"多少 P":取短边。
    // **不能直接用 height**:竖屏视频(1080x1920)的 height 是 1920 ——
    // 用户说的"1080P"和网站标的都是短边,拿长边会显示成 1920P。
    int shortSide() const
    {
        if (width > 0 && height > 0)
            return width < height ? width : height;
        return height;
    }
    // HDR(含杜比视界):本地 Qt 播放器对它需要离线转 SDR,见 HdrConverter
    bool isHdr() const;

    // 体积:优先确切大小,没有就退回估算值;都拿不到返回 0
    qint64 bestSize() const { return filesize > 0 ? filesize : filesizeApprox; }

    // 展示名:"1080P60 · H.264 · 1.5 Mbps · HDR · 约 48.8 MB"
    // (纯音频是 "音频 · AAC · 151 kbps · 立体声 · 约 4.8 MB")
    // **拼装放在这里**,QML 只负责画 —— 免得两边各拼一套、慢慢跑偏
    QString displayName() const;

    // 简短的编码家族名:"H.264" / "HEVC" / "AV1" / "AAC" / "Opus" ...
    QString videoCodecName() const;
    QString audioCodecName() const;
};

// 一集(剧集类链接的 entries[] 里的一项)。
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

// 一次探测的全貌(title 这些通用字段也收着,以后做站点特化时再往这里加)
struct MediaInfo {
    QString url;
    QString title;
    QString uploader;
    qint64 durationMs = 0;
    bool live = false;       // 正在直播(没有时长、不能下载合并)
    bool wasLive = false;    // 直播回放:有时长,当普通视频处理
    QList<MediaFormat> formats;

    // 多集(yt-dlp 的 _type == "playlist"):顶层没有 formats,每一集自带。
    // 两条判据:**顶层 formats 为空** + episodes 非空 —— 单视频永远没有 episodes
    QList<Episode> episodes;
    QString playlistTitle;   // 合集/番剧的名字("学生会的一己之见")

    bool isEmpty() const { return formats.isEmpty() && title.isEmpty(); }
    bool isPlaylist() const { return !episodes.isEmpty(); }
};

// 简单视图的一行(分辨率档)。label 是主文字,detail 是右边的小字。
struct QualityOption {
    // 选它时条目上记的 spec。
    // 直播:该 variant 的 m3u8 地址;普通视频:高度上限的数字("1080",0 = 不设限)
    QString spec;
    QString label;     // "1080P60"
    QString detail;    // "H.264 · 1.5 Mbps";直播时是协议名
    int height = 0;    // 0 = 最高画质(不设限)
    bool hasHdr = false; // 这一档只有 HDR 可选(界面给个提示)
};

// 详情视图的一行:一条具体的流,点它就等于"手选这条"
struct FormatChoice {
    QString formatId;
    QString title;     // "1080x1920 · 30fps · H.264 高码率"
    QString detail;    // "1.5 Mbps · 约 48.8 MB"
    int role = 0;      // 0=视频 1=音频 2=合一
    bool hdr = false;
};

// 按 spec 挑出来的结果
struct MediaSelection {
    bool valid = false;
    bool live = false;          // 直播:不下载,直接拉流
    bool direct = false;        // 选中的是合一格式,能把 url 直接交给播放器
    QString url;                // direct 时的直链(直播时是 m3u8)
    QString formatExpression;   // yt-dlp 的 -f 表达式
    QStringList sortArgs;       // yt-dlp 的 -S 排序参数(apps 在一起)
    QString label;              // "1080P60 · H.264",给日志和界面用
    bool hdr = false;           // 选中的带 HDR —— 界面要能提前提醒
    MediaFormat video;          // 选中的画面流(直播时为空)
    MediaFormat audio;          // 选中的声音流(合一格式时等于 video)
};

// 条目上 spec 的编解码与规则。字符串是跨客户端的"共同约定",见文件头。
namespace StreamSpec {

// spec 是否带 HDR 后缀(":h")
bool hdrAllowed(const QString &spec);
// 去掉后缀,只留主体("1080:h" → "1080")
QString body(const QString &spec);
// 主体转成数字(注意是 formats 的 **height 字段**,竖屏 1080P 那条是 1920);
// 不是数字(手选 format_id / 空串)返回 -1,空串单独返回 0
int heightOf(const QString &spec);
// 给界面看的短标签:"最高画质" / "1080P" / "手选 1080P";hdr 时后面缀 "+HDR"。
// 数字档位要显示成短边(1080P)得对着 formats 找回真正的流 —— 所以 info 传空
// 时只能原样显示("1920P"),列表那边就是这样,播放页传完整 info 才准
QString label(const QString &spec, const MediaInfo &info);
// 把界面选中的档位/手选流拼成带 HDR 后缀的 spec
QString withHdr(const QString &specBody, bool allowHdr);

} // namespace StreamSpec

namespace MediaFormats {

// yt-dlp 的 formats[] → MediaFormat 列表(丢弃故事板之类的非媒体项)
QList<MediaFormat> parse(const QJsonArray &formats);

// 整份 `-J` 输出 → MediaInfo。title/uploader/duration/live 都在这里收
MediaInfo parseMediaInfo(const QJsonObject &root);

// ---- 简单视图 ----
// 按**画面高度**归纳成档位:同一高度多条(编码不同)合成一行,
// 代表是"该档里优先级最高的那条"(优先级规则见 pick)。从高到低排序。
QList<QualityOption> qualityOptions(const MediaInfo &info);

// ---- 详情视图 ----
// 每一条流一行,按 视频 → 音频 → 合一 分好组、组内按优先级排序
QList<FormatChoice> detailChoices(const MediaInfo &info);

// ---- 剧集:itemId 和标题的生成规则 ----
// 这两条放这里(而不是对话框里),是因为它们是**跨客户端的约定**:
// 同一个 ep id 在哪台机器上都要翻成同一个 itemId,否则同一集在房间列表里
// 会被当成两条(服务端查重按 itemId 比)。
namespace Episodes {

// 这一集的房间条目 id:"ep<平台编号>";编号拿不到时退回用 URL 的哈希兜底(仍然确定)
QString itemId(const Episode &episode);
// 列表里显示的名字:"第 N 集 · 标题"(标题已含集号时不再重复加前缀)
QString displayTitle(const Episode &episode, int fallbackIndex);

} // namespace Episodes

// ---- 播放/下载 ----
// 按 spec 解析成要下的流。live 的 formats 是清单里的 variant:
// 空 spec 给 master 清单(播放器自适应),具体 id 给对应 variant。
MediaSelection resolve(const QString &spec, const MediaInfo &info);

} // namespace MediaFormats

#endif //SYNCINE_FORMATMODEL_H
