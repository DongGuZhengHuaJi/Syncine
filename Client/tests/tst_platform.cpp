// 平台注册表与 B 站编号展开的纯逻辑测试。

#include <QCoreApplication>
#include <QUrl>

#include <cstdio>

#include "media/MediaPlatform.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

QString normalize(const QString &platformId, const QString &raw, QString *error)
{
    return mediaPlatformRegistry().normalize(platformId, raw, error);
}

void testDetect()
{
    const auto &registry = mediaPlatformRegistry();

    CHECK(registry.detect(QUrl(QStringLiteral("https://www.bilibili.com/video/BV11FJv6ZEmH")))->id()
          == QStringLiteral("bilibili"));
    CHECK(registry.detect(QUrl(QStringLiteral("https://live.bilibili.com/9576151")))->id()
          == QStringLiteral("bilibili"));
    CHECK(registry.detect(QUrl(QStringLiteral("https://b23.tv/abc")))->id()
          == QStringLiteral("bilibili"));
    CHECK(registry.detect(QUrl(QStringLiteral("https://www.youtube.com/watch?v=x")))->id()
          == QStringLiteral("generic"));
    // 不能按 contains 匹配。
    CHECK(registry.detect(QUrl(QStringLiteral("https://evilbilibili.com/video/BV11FJv6ZEmH")))->id()
          == QStringLiteral("generic"));
}

void testBilibiliIds()
{
    QString error;

    CHECK(normalize(QStringLiteral("bilibili"), QStringLiteral("BV11FJv6ZEmH"), &error)
          == QStringLiteral("https://www.bilibili.com/video/BV11FJv6ZEmH"));
    CHECK(error.isEmpty());

    CHECK(normalize(QStringLiteral("bilibili"), QStringLiteral("av170001"), &error)
          == QStringLiteral("https://www.bilibili.com/video/av170001"));

    CHECK(normalize(QStringLiteral("bilibili"), QStringLiteral("ss1588"), &error)
          == QStringLiteral("https://www.bilibili.com/bangumi/play/ss1588"));

    CHECK(normalize(QStringLiteral("bilibili"), QStringLiteral("ep29144"), &error)
          == QStringLiteral("https://www.bilibili.com/bangumi/play/ep29144"));

    // 纯数字按直播间号。
    CHECK(normalize(QStringLiteral("bilibili"), QStringLiteral("9576151"), &error)
          == QStringLiteral("https://live.bilibili.com/9576151"));

    // 无 scheme 的域名补 https；末尾 / 与 fragment 被去掉。
    CHECK(normalize(QStringLiteral("bilibili"),
                    QStringLiteral("bilibili.com/video/BV11FJv6ZEmH/#reply"), &error)
          == QStringLiteral("https://www.bilibili.com/video/BV11FJv6ZEmH"));

    // 分 P / 时间参数保留。
    CHECK(normalize(QStringLiteral("bilibili"),
                    QStringLiteral("https://www.bilibili.com/video/BV11FJv6ZEmH?p=2"), &error)
          == QStringLiteral("https://www.bilibili.com/video/BV11FJv6ZEmH?p=2"));

    // 无法识别 / 非 B 站链接。
    CHECK(normalize(QStringLiteral("bilibili"), QStringLiteral("hello world"), &error).isEmpty());
    CHECK(!error.isEmpty());
    CHECK(normalize(QStringLiteral("bilibili"),
                    QStringLiteral("https://www.youtube.com/watch?v=x"), &error).isEmpty());
}

void testGeneric()
{
    QString error;

    CHECK(normalize(QStringLiteral("generic"),
                    QStringLiteral("https://example.com/video/1"), &error)
          == QStringLiteral("https://example.com/video/1"));

    // 省略 scheme 的域名可以补全。
    CHECK(normalize(QStringLiteral("generic"),
                    QStringLiteral("example.com/video/1"), &error)
          == QStringLiteral("https://example.com/video/1"));

    // 裸 B 站编号不属于 Generic。
    CHECK(normalize(QStringLiteral("generic"), QStringLiteral("BV11FJv6ZEmH"), &error).isEmpty());
    CHECK(!error.isEmpty());

    // 不指定来源时自动识别编号。
    CHECK(normalize(QString(), QStringLiteral("ep29144"), &error)
          == QStringLiteral("https://www.bilibili.com/bangumi/play/ep29144"));
}

void testCookiePolicy()
{
    const MediaPlatform *bilibili = mediaPlatformRegistry().byId(QStringLiteral("bilibili"));
    const MediaPlatform *generic = mediaPlatformRegistry().byId(QStringLiteral("generic"));
    CHECK(platformUsesCookies(bilibili, false));
    CHECK(!platformUsesCookies(generic, false));
    CHECK(platformUsesCookies(generic, true));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testDetect();
    testBilibiliIds();
    testGeneric();
    testCookiePolicy();

    if (g_failures == 0) {
        std::printf("platform: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "platform: %d check(s) failed\n", g_failures);
    return 1;
}
