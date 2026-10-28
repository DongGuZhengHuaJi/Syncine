// FormatModel / StreamSpec 的纯逻辑回归测试。
//
// 不依赖 QML、网络或 yt-dlp：手工构造 MediaInfo，验证 spec 解释优先级。
// 重点覆盖「手选纯数字 format_id 被当成档位」这个已修复的 bug。

#include <QCoreApplication>

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
    testLabels();

    if (g_failures == 0) {
        std::printf("formatmodel: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "formatmodel: %d check(s) failed\n", g_failures);
    return 1;
}
