// YtDlpOutput 的纯逻辑测试：混合输出中的 JSON 提取与 ERROR 行选择。

#include <QCoreApplication>
#include <QJsonObject>

#include <cstdio>

#include "core/YtDlpOutput.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // 纯 JSON
    QJsonObject object = YtDlpOutput::parseJsonObject("{\"a\": 1}\n");
    CHECK(object.value(QStringLiteral("a")).toInt() == 1);

    // 警告前缀 + pretty JSON
    object = YtDlpOutput::parseJsonObject(
        "WARNING: something happened\n"
        "{\n"
        "  \"b\": 2\n"
        "}\n");
    CHECK(object.value(QStringLiteral("b")).toInt() == 2);

    // 警告文本里也含 '{'，不能从那里开始解析
    object = YtDlpOutput::parseJsonObject(
        "WARNING: invalid map {not json}\n"
        "{\n"
        "  \"c\": 3\n"
        "}\n");
    CHECK(object.value(QStringLiteral("c")).toInt() == 3);

    // 完全不是 JSON
    CHECK(YtDlpOutput::parseJsonObject("ERROR: nope\n").isEmpty());

    // ERROR 行：取最后一条；没有就用 fallback/整段
    const QStringList lines{
        QStringLiteral("WARNING: x"),
        QStringLiteral("ERROR: first"),
        QStringLiteral("more"),
        QStringLiteral("ERROR: last"),
    };
    CHECK(YtDlpOutput::lastError(lines, QStringLiteral("fallback"))
          == QStringLiteral("ERROR: last"));
    CHECK(YtDlpOutput::lastError({QStringLiteral("plain")},
                                 QStringLiteral("fallback"))
          == QStringLiteral("plain"));
    CHECK(YtDlpOutput::lastError({}, QStringLiteral("fallback"))
          == QStringLiteral("fallback"));

    if (g_failures == 0) {
        std::printf("ytdlpoutput: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "ytdlpoutput: %d check(s) failed\n", g_failures);
    return 1;
}
