// FormatModel / StreamSpec 的纯逻辑回归测试。
//
// 不依赖 QML、网络或 yt-dlp：手工构造 MediaInfo，验证 spec 解释优先级。
// 重点覆盖「手选纯数字 format_id 被当成档位」这个已修复的 bug。

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

#include <cstdio>

#include "media/FormatModel.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

MediaFormat makeVideo(const QString &id, int height, const QString &vcodec = QStringLiteral("h264"))
{
    MediaFormat format;
    format.valid = true;
    format.formatId = id;
    format.kind = MediaFormat::VideoOnly;
    format.width = height * 16 / 9;
    format.height = height;
    format.fps = 30;
    format.vcodec = vcodec;
    format.acodec = QStringLiteral("none");
    format.url = QStringLiteral("https://example.invalid/v/") + id;
    format.protocol = QStringLiteral("https");
    return format;
}

MediaFormat makeAudio(const QString &id)
{
    MediaFormat format;
    format.valid = true;
    format.formatId = id;
    format.kind = MediaFormat::AudioOnly;
    format.vcodec = QStringLiteral("none");
    format.acodec = QStringLiteral("aac");
    format.abr = 128;
    format.url = QStringLiteral("https://example.invalid/a/") + id;
    format.protocol = QStringLiteral("https");
    return format;
}

MediaFormat makeCombined(const QString &id, int height)
{
    MediaFormat format = makeVideo(id, height);
    format.kind = MediaFormat::Combined;
    format.acodec = QStringLiteral("aac");
    return format;
}

MediaInfo sampleInfo()
{
    MediaInfo info;
    info.title = QStringLiteral("test");
    info.formats = {
        makeVideo(QStringLiteral("30080"), 1080),
        makeVideo(QStringLiteral("30102"), 1080, QStringLiteral("hev1")),
        makeVideo(QStringLiteral("137"), 2160),
        makeVideo(QStringLiteral("1080"), 480), // 故意让 format_id 与档位数字撞名
        makeAudio(QStringLiteral("30280")),
        makeCombined(QStringLiteral("18"), 720),
    };
    return info;
}

void testNumericFormatId()
{
    const MediaInfo info = sampleInfo();

    // 30080 不是源里的高度，但确实是 format_id → 必须按 id 选，而不是当档位。
    MediaSelection selected = MediaFormats::resolve(QStringLiteral("30080"), info, true);
    CHECK(selected.valid);
    CHECK(selected.video.formatId == QStringLiteral("30080"));
    CHECK(selected.formatExpression.contains(QStringLiteral("30080")));
    CHECK(selected.formatExpression.contains(QStringLiteral("30280")));

    // 137 同理（低于 1080 的纯数字 id 不能被当成分辨率上限）。
    selected = MediaFormats::resolve(QStringLiteral("137"), info, true);
    CHECK(selected.valid);
    CHECK(selected.video.formatId == QStringLiteral("137"));
}

void testTierWinsWhenHeightExists()
{
    const MediaInfo info = sampleInfo();

    // "1080" 同时是档位数字和一个 format_id；因为源里确实存在 1080 高度，
    // 必须先按档位解释（否则旧条目的档位 spec 会被改变）。
    const MediaSelection selected = MediaFormats::resolve(QStringLiteral("1080"), info, true);
    CHECK(selected.valid);
    CHECK(selected.video.height == 1080);
    CHECK(selected.video.formatId != QStringLiteral("1080"));
}

void testExplicitIdPrefix()
{
    const MediaInfo info = sampleInfo();

    MediaSelection selected = MediaFormats::resolve(QStringLiteral("id:137"), info, true);
    CHECK(selected.valid);
    CHECK(selected.video.formatId == QStringLiteral("137"));

    // 显式 id 不存在 → 报错，不能静默回退到最高画质。
    selected = MediaFormats::resolve(QStringLiteral("id:999999"), info, true);
    CHECK(!selected.valid);
    CHECK(selected.error.contains(QStringLiteral("不存在")));
}

void testLegacyPair()
{
    const MediaInfo info = sampleInfo();
    const MediaSelection selected =
        MediaFormats::resolve(QStringLiteral("30102+30280"), info, true);
    CHECK(selected.valid);
    CHECK(selected.video.formatId == QStringLiteral("30102"));
    CHECK(selected.audio.formatId == QStringLiteral("30280"));
    CHECK(selected.formatExpression == QStringLiteral("30102+30280"));
}

QJsonObject pluginHlsFormat()
{
    QJsonObject format;
    format.insert(QStringLiteral("format_id"), QStringLiteral("mdzy-4064"));
    format.insert(QStringLiteral("url"),
                  QStringLiteral("https://cdn.example.invalid/4064kb/hls/index.m3u8"));
    format.insert(QStringLiteral("protocol"), QStringLiteral("m3u8_native"));
    format.insert(QStringLiteral("ext"), QStringLiteral("mp4"));
    format.insert(QStringLiteral("video_ext"), QStringLiteral("mp4"));
    format.insert(QStringLiteral("audio_ext"), QStringLiteral("none"));
    format.insert(QStringLiteral("width"), 1920);
    format.insert(QStringLiteral("height"), 1080);
    format.insert(QStringLiteral("dynamic_range"), QStringLiteral("SDR"));
    format.insert(QStringLiteral("tbr"), 4064.0);
    return format;
}

// 插件返回的点播 HLS：vcodec/acodec 为 null、duration 为空。
// 不能因为 formats 里有 m3u8 就当成直播。
void testPluginVodHlsIsNotLive()
{
    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("video"));
    root.insert(QStringLiteral("id"), QStringLiteral("2026838825_ep10"));
    root.insert(QStringLiteral("title"), QStringLiteral("plugin episode"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://omofun.in/vod/play/2026838825/ep10.html"));
    root.insert(QStringLiteral("duration"), QJsonValue::Null);
    root.insert(QStringLiteral("formats"), QJsonArray{pluginHlsFormat()});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(!info.live);
    CHECK(info.formats.size() == 1);
    CHECK(info.formats.first().kind == MediaFormat::Combined);

    const MediaSelection selected =
        MediaFormats::resolve(QStringLiteral("1080"), info, true);
    CHECK(selected.valid);
    CHECK(!selected.direct);
    CHECK(selected.video.formatId == QStringLiteral("mdzy-4064"));
}

// flat playlist 条目只有 url（没有 webpage_url），插件用 url_result 时也是这种。
void testFlatPlaylistUrlFallback()
{
    QJsonObject entry;
    entry.insert(QStringLiteral("_type"), QStringLiteral("url"));
    entry.insert(QStringLiteral("id"), QStringLiteral("2026838825_ep1"));
    entry.insert(QStringLiteral("url"),
                 QStringLiteral("https://omofun.in/vod/play/2026838825/ep1.html"));

    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("playlist"));
    root.insert(QStringLiteral("title"), QStringLiteral("2026838825"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://omofun.in/vod/detail/2026838825.html"));
    root.insert(QStringLiteral("entries"), QJsonArray{entry});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(info.isPlaylist());
    CHECK(info.episodes.size() == 1);
    CHECK(info.episodes.first().url
          == QStringLiteral("https://omofun.in/vod/play/2026838825/ep1.html"));
    CHECK(info.episodes.first().id == QStringLiteral("2026838825_ep1"));
    CHECK(MediaFormats::Episodes::displayTitle(info.episodes.first(), 1)
          == QStringLiteral("第 1 集"));
}

// 裸 m3u8 仍按直播兜底（源地址本身就是 HLS 清单）。
void testBareM3u8StillLive()
{
    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("video"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://example.invalid/live/index.m3u8"));
    root.insert(QStringLiteral("formats"), QJsonArray{pluginHlsFormat()});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(info.live);

    const MediaSelection selected = MediaFormats::resolve(QString(), info, true);
    CHECK(selected.valid);
    CHECK(selected.direct);
}

// 部分 extractor 只给 resolution，不单独给 width/height。
void testResolutionFallback()
{
    QJsonObject format = pluginHlsFormat();
    format.remove(QStringLiteral("width"));
    format.remove(QStringLiteral("height"));
    format.insert(QStringLiteral("resolution"), QStringLiteral("1920x1080"));

    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("video"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://example.invalid/vod/ep1.html"));
    root.insert(QStringLiteral("duration"), 60.0);
    root.insert(QStringLiteral("formats"), QJsonArray{format});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(info.formats.size() == 1);
    CHECK(info.formats.first().width == 1920);
    CHECK(info.formats.first().height == 1080);
}

// 自定义 extractor 可能不写 protocol：HLS 地址要能按 URL 推断出来。
void testMissingProtocolInferredFromHlsUrl()
{
    QJsonObject format = pluginHlsFormat();
    format.remove(QStringLiteral("protocol"));

    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("video"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://example.invalid/vod/ep1.html"));
    root.insert(QStringLiteral("duration"), QJsonValue::Null);
    root.insert(QStringLiteral("formats"), QJsonArray{format});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(info.formats.size() == 1);
    CHECK(info.formats.first().protocol == QStringLiteral("m3u8_native"));
    CHECK(!info.live);
}

// 手选合一 HLS 时，url 为空要回退 manifest_url。
void testCombinedManifestUrlFallback()
{
    QJsonObject format = pluginHlsFormat();
    format.remove(QStringLiteral("url"));
    format.insert(QStringLiteral("manifest_url"),
                  QStringLiteral("https://cdn.example.invalid/master.m3u8"));

    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("video"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://example.invalid/vod/ep1.html"));
    root.insert(QStringLiteral("duration"), 60.0);
    root.insert(QStringLiteral("formats"), QJsonArray{format});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    const MediaSelection selected =
        MediaFormats::resolve(QStringLiteral("id:mdzy-4064"), info, true);
    CHECK(selected.valid);
    CHECK(selected.direct);
    CHECK(selected.url == QStringLiteral("https://cdn.example.invalid/master.m3u8"));
}

// 插件不提供 format_id 时，流仍然是有效的，应能通过 -S 选择。
void testFormatWithoutIdIsSelectable()
{
    MediaInfo info;
    info.title = QStringLiteral("no id");
    info.formats = {makeVideo(QString(), 720)};

    const MediaSelection selected = MediaFormats::resolve(QStringLiteral("720"), info, true);
    CHECK(selected.valid);
    CHECK(selected.video.valid);
    CHECK(selected.video.formatId.isEmpty());
    CHECK(selected.formatExpression.isEmpty());
    CHECK(!selected.sortArgs.isEmpty());
}

// 单格式信息可能只有顶层 url/format_id，没有 formats[]。
void testTopLevelUrlBecomesFormat()
{
    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("video"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://example.invalid/v/1"));
    root.insert(QStringLiteral("url"),
                QStringLiteral("https://cdn.example.invalid/media.mp4"));
    root.insert(QStringLiteral("format_id"), QStringLiteral("0"));
    root.insert(QStringLiteral("ext"), QStringLiteral("mp4"));
    root.insert(QStringLiteral("video_ext"), QStringLiteral("mp4"));
    root.insert(QStringLiteral("audio_ext"), QStringLiteral("none"));
    root.insert(QStringLiteral("width"), 1280);
    root.insert(QStringLiteral("height"), 720);

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(info.formats.size() == 1);
    CHECK(info.formats.first().valid);
    CHECK(info.formats.first().kind == MediaFormat::Combined);
    CHECK(info.formats.first().isDirectPlayable());
}

void testPlaylistWithTopLevelUrlIsNotSingleVideo()
{
    QJsonObject entry;
    entry.insert(QStringLiteral("id"), QStringLiteral("ep1"));
    entry.insert(QStringLiteral("url"),
                 QStringLiteral("https://example.invalid/play/ep1.html"));

    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("playlist"));
    root.insert(QStringLiteral("title"), QStringLiteral("season"));
    root.insert(QStringLiteral("url"), QStringLiteral("https://example.invalid/playlist"));
    root.insert(QStringLiteral("entries"), QJsonArray{entry});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(info.formats.isEmpty());
    CHECK(info.isPlaylist());
    CHECK(info.episodes.size() == 1);
    CHECK(info.episodes.first().url
          == QStringLiteral("https://example.invalid/play/ep1.html"));
}

void testRequestedFormatsFallback()
{
    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("video"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://example.invalid/v/1"));
    root.insert(QStringLiteral("duration"), 60.0);
    root.insert(QStringLiteral("requested_formats"), QJsonArray{pluginHlsFormat()});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(info.formats.size() == 1);
    CHECK(info.formats.first().formatId == QStringLiteral("mdzy-4064"));
}

void testStringNumbersAndDurationString()
{
    QJsonObject format = pluginHlsFormat();
    format.insert(QStringLiteral("width"), QStringLiteral("1920"));
    format.insert(QStringLiteral("height"), QStringLiteral("1080"));
    format.insert(QStringLiteral("tbr"), QStringLiteral("4064"));
    format.insert(QStringLiteral("fps"), QStringLiteral("30"));

    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("video"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://example.invalid/v/1"));
    root.insert(QStringLiteral("duration_string"), QStringLiteral("1:02:03"));
    root.insert(QStringLiteral("formats"), QJsonArray{format});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(info.durationMs == 3723000);
    CHECK(info.formats.size() == 1);
    CHECK(info.formats.first().width == 1920);
    CHECK(info.formats.first().height == 1080);
    CHECK(info.formats.first().tbr == 4064);
    CHECK(info.formats.first().fps == 30);
}

void testResolutionVariants()
{
    QJsonObject byPair = pluginHlsFormat();
    byPair.remove(QStringLiteral("width"));
    byPair.remove(QStringLiteral("height"));
    byPair.insert(QStringLiteral("resolution"),
                  QStringLiteral("1920x1080, 1080p"));

    QJsonObject byNote = pluginHlsFormat();
    byNote.remove(QStringLiteral("width"));
    byNote.remove(QStringLiteral("height"));
    byNote.insert(QStringLiteral("format_note"), QStringLiteral("1080P 高码率"));

    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("video"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://example.invalid/v/1"));
    root.insert(QStringLiteral("duration"), 60.0);
    root.insert(QStringLiteral("formats"), QJsonArray{byPair, byNote});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(info.formats.size() == 2);
    CHECK(info.formats.at(0).width == 1920);
    CHECK(info.formats.at(0).height == 1080);
    CHECK(info.formats.at(1).width == 0);
    CHECK(info.formats.at(1).height == 1080);
}

void testLiveStatusFallbacks()
{
    QJsonObject live;
    live.insert(QStringLiteral("_type"), QStringLiteral("video"));
    live.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://example.invalid/live/page"));
    live.insert(QStringLiteral("live_status"), QStringLiteral("is_live"));
    live.insert(QStringLiteral("formats"), QJsonArray{pluginHlsFormat()});

    const MediaInfo liveInfo = MediaFormats::parseMediaInfo(live);
    CHECK(liveInfo.live);

    QJsonObject numericLive;
    numericLive.insert(QStringLiteral("_type"), QStringLiteral("video"));
    numericLive.insert(QStringLiteral("webpage_url"),
                       QStringLiteral("https://example.invalid/live/page2"));
    numericLive.insert(QStringLiteral("is_live"), 1);
    numericLive.insert(QStringLiteral("formats"), QJsonArray{pluginHlsFormat()});
    CHECK(MediaFormats::parseMediaInfo(numericLive).live);

    QJsonObject wasLive;
    wasLive.insert(QStringLiteral("_type"), QStringLiteral("video"));
    wasLive.insert(QStringLiteral("webpage_url"),
                   QStringLiteral("https://example.invalid/v/1"));
    wasLive.insert(QStringLiteral("live_status"), QStringLiteral("was_live"));
    wasLive.insert(QStringLiteral("duration"), 60.0);
    wasLive.insert(QStringLiteral("formats"), QJsonArray{pluginHlsFormat()});

    const MediaInfo wasLiveInfo = MediaFormats::parseMediaInfo(wasLive);
    CHECK(wasLiveInfo.wasLive);
    CHECK(!wasLiveInfo.live);
}

void testDashNotDirectPlayable()
{
    MediaFormat format;
    format.protocol = QStringLiteral("http_dash_segments");
    CHECK(!format.isDirectPlayable());

    format.protocol = QStringLiteral("https");
    CHECK(format.isDirectPlayable());

    format.protocol = QStringLiteral("m3u8_native");
    CHECK(format.isDirectPlayable());
}

void testMissingProtocolDash()
{
    QJsonObject format = pluginHlsFormat();
    format.insert(QStringLiteral("url"),
                  QStringLiteral("https://cdn.example.invalid/manifest.mpd"));
    format.remove(QStringLiteral("protocol"));

    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("video"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://example.invalid/v/1"));
    root.insert(QStringLiteral("duration"), 60.0);
    root.insert(QStringLiteral("formats"), QJsonArray{format});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(info.formats.size() == 1);
    CHECK(info.formats.first().protocol == QStringLiteral("http_dash_segments"));
    CHECK(!info.formats.first().isDirectPlayable());
}

void testAudioOnlyInference()
{
    QJsonObject byAudioExt = pluginHlsFormat();
    byAudioExt.insert(QStringLiteral("url"),
                      QStringLiteral("https://cdn.example.invalid/audio.m4a"));
    byAudioExt.insert(QStringLiteral("ext"), QStringLiteral("m4a"));
    byAudioExt.insert(QStringLiteral("video_ext"), QStringLiteral("none"));
    byAudioExt.insert(QStringLiteral("audio_ext"), QStringLiteral("m4a"));
    byAudioExt.remove(QStringLiteral("protocol"));

    QJsonObject byContainerExt = pluginHlsFormat();
    byContainerExt.insert(QStringLiteral("url"),
                          QStringLiteral("https://cdn.example.invalid/audio2.m4a"));
    byContainerExt.insert(QStringLiteral("ext"), QStringLiteral("m4a"));
    byContainerExt.remove(QStringLiteral("video_ext"));
    byContainerExt.remove(QStringLiteral("audio_ext"));
    byContainerExt.remove(QStringLiteral("protocol"));

    QJsonObject root;
    root.insert(QStringLiteral("_type"), QStringLiteral("video"));
    root.insert(QStringLiteral("webpage_url"),
                QStringLiteral("https://example.invalid/v/1"));
    root.insert(QStringLiteral("duration"), 60.0);
    root.insert(QStringLiteral("formats"), QJsonArray{byAudioExt, byContainerExt});

    const MediaInfo info = MediaFormats::parseMediaInfo(root);
    CHECK(info.formats.size() == 2);
    CHECK(info.formats.at(0).kind == MediaFormat::AudioOnly);
    CHECK(info.formats.at(1).kind == MediaFormat::AudioOnly);
}

void testDisplayTitleAvoidsDuplicateNumber()
{
    Episode episode;
    episode.title = QStringLiteral("葬送的芙莉莲 第二季 第1集");
    episode.number = 1;
    CHECK(MediaFormats::Episodes::displayTitle(episode, 1) == episode.title);
}

void testLabels()
{
    const MediaInfo info = sampleInfo();

    CHECK(StreamSpec::label(QStringLiteral("id:30080"), MediaInfo())
          == QStringLiteral("手选"));
    CHECK(StreamSpec::label(QStringLiteral("id:30102+30280"), info)
          == QStringLiteral("手选 1080P"));

    // 旧裸数字 id 不能显示成 "30080P"；标准档位仍按 P 显示。
    CHECK(!StreamSpec::label(QStringLiteral("30080"), MediaInfo())
               .contains(QStringLiteral("30080P")));
    CHECK(StreamSpec::label(QStringLiteral("1080"), MediaInfo())
          == QStringLiteral("1080P"));

    CHECK(StreamSpec::heightOf(QStringLiteral("id:137")) < 0);
    CHECK(StreamSpec::heightOf(QStringLiteral("1080")) == 1080);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testNumericFormatId();
    testTierWinsWhenHeightExists();
    testExplicitIdPrefix();
    testLegacyPair();
    testPluginVodHlsIsNotLive();
    testFlatPlaylistUrlFallback();
    testBareM3u8StillLive();
    testResolutionFallback();
    testMissingProtocolInferredFromHlsUrl();
    testCombinedManifestUrlFallback();
    testFormatWithoutIdIsSelectable();
    testTopLevelUrlBecomesFormat();
    testPlaylistWithTopLevelUrlIsNotSingleVideo();
    testRequestedFormatsFallback();
    testStringNumbersAndDurationString();
    testResolutionVariants();
    testLiveStatusFallbacks();
    testDashNotDirectPlayable();
    testMissingProtocolDash();
    testAudioOnlyInference();
    testDisplayTitleAvoidsDuplicateNumber();
    testLabels();

    if (g_failures == 0) {
        std::printf("formatmodel: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "formatmodel: %d check(s) failed\n", g_failures);
    return 1;
}
