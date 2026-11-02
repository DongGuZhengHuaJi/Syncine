// Toolchain 的 yt-dlp 插件参数回归测试。
// 只验证参数构造，不启动 yt-dlp 进程。

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cstdio>

#include "core/Toolchain.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

QStringList pluginDirs(const QStringList &args)
{
    QStringList dirs;
    for (int i = 0; i + 1 < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--plugin-dirs"))
            dirs << args.at(i + 1);
    }
    return dirs;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    Toolchain toolchain;

    // 内置插件（Client/plugins）必须始终被交给 yt-dlp，即使“用户插件”开关关闭。
    const QString bundled = toolchain.bundledPluginDirectory();
    CHECK(!bundled.isEmpty());
    CHECK(QDir(bundled).isAbsolute());

    // 默认关闭用户插件：只带 --no-plugin-dirs + 内置插件目录
    QStringList args = toolchain.pluginArgs();
    CHECK(args.contains(QStringLiteral("--no-plugin-dirs")));
    QStringList dirs = pluginDirs(args);
    CHECK(dirs.contains(QDir(bundled).absolutePath()));
    CHECK(dirs.size() == 1);

    // 启用但用户目录不存在：不能传它（yt-dlp 会直接退出），内置目录仍在
    toolchain.setPluginsEnabled(true);
    toolchain.setPluginDirectory(QStringLiteral("/path/does/not/exist/syncine-test"));
    args = toolchain.pluginArgs();
    CHECK(args.contains(QStringLiteral("--no-plugin-dirs")));
    dirs = pluginDirs(args);
    CHECK(dirs.contains(QDir(bundled).absolutePath()));
    CHECK(!dirs.contains(QDir(QStringLiteral("/path/does/not/exist/syncine-test"))
                             .absolutePath()));

    // 用户目录存在：内置目录在前，用户目录在后
    QTemporaryDir temp;
    CHECK(temp.isValid());
    const QString pluginDir = temp.path() + QStringLiteral("/plugins");
    CHECK(QDir().mkpath(pluginDir));

    toolchain.setPluginDirectory(pluginDir);
    args = toolchain.pluginArgs();
    const int noIndex = args.indexOf(QStringLiteral("--no-plugin-dirs"));
    const int pluginIndex = args.indexOf(QStringLiteral("--plugin-dirs"));
    CHECK(noIndex >= 0);
    CHECK(pluginIndex > noIndex);
    dirs = pluginDirs(args);
    CHECK(dirs.value(0) == QDir(bundled).absolutePath());
    CHECK(dirs.value(1) == QDir(pluginDir).absolutePath());
    CHECK(dirs.size() == 2);

    // 关闭用户插件后，用户目录不再出现；内置目录不受影响
    toolchain.setPluginsEnabled(false);
    args = toolchain.pluginArgs();
    CHECK(args.contains(QStringLiteral("--no-plugin-dirs")));
    dirs = pluginDirs(args);
    CHECK(dirs.contains(QDir(bundled).absolutePath()));
    CHECK(!dirs.contains(QDir(pluginDir).absolutePath()));
    CHECK(dirs.size() == 1);

    if (g_failures == 0) {
        std::printf("toolchain: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "toolchain: %d check(s) failed\n", g_failures);
    return 1;
}
