//
// 媒体内存探针 —— 验收工具，不是单元测试。
//
// 用真实的 PlaybackController 跑「加载 → 解码 → 卸载」循环，观察两件事：
//   1. unload() 之后 QVideoSink 里只剩 2×2 的填充帧，没有继续攥着 4K 最后一帧；
//   2. 重复加载同一段视频时，每轮卸载后的 RSS 不随轮次持续单调增长。
//
// 用法：
//   SYNCINE_MEMORY_VIDEO=/path/to/video.mp4 ./syncine_memory_probe
//   SYNCINE_MEMORY_ROUNDS=20          # 轮数（默认 20，建议验收时 50）
//
// 没设 SYNCINE_MEMORY_VIDEO 时直接跳过（返回 0）：CI 里没有测试素材，
// 不能让它变成必失败项。
//
// 为什么不用单元断言 RSS 的绝对大小：Qt Multimedia / FFmpeg 的高水位是
// 底层行为（见 docs/Syncine-视频加载内存问题与修复方案-2026-10-29.md），
// 允许涨到平台，只要求"不随轮次持续增长"。
//

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QThread>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

#if defined(__linux__)
// rssKb() 读 /proc 并用 sysconf 取页大小
#include <unistd.h>
#endif

#include "playback/PlaybackController.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

// 当前进程 RSS（KB）。仅 Linux 可读（statm 第二列是常驻页数）；
// 其它平台返回 -1，探针退化成只检查"最后一帧是否释放"。
qint64 rssKb()
{
#if defined(__linux__)
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (!statm.open(QIODevice::ReadOnly))
        return -1;
    const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
    if (fields.size() < 2)
        return -1;
    bool ok = false;
    const qint64 pages = fields.at(1).toLongLong(&ok);
    if (!ok)
        return -1;
    return pages * (qint64(sysconf(_SC_PAGESIZE)) / 1024);
#else
    return -1;
#endif
}

// 跑事件循环直到条件成立或超时。
bool waitFor(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

QString formatMb(qint64 kb)
{
    if (kb < 0)
        return QStringLiteral("?");
    return QString::number(kb / 1024.0, 'f', 1);
}

} // namespace

int main(int argc, char **argv)
{
    // 探针不需要真实窗口；没有桌面环境时也能跑。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QCoreApplication app(argc, argv);

    const QString videoPath = qEnvironmentVariable("SYNCINE_MEMORY_VIDEO");
    if (videoPath.isEmpty() || !QFile::exists(videoPath)) {
        std::printf("media-memory: 未设置 SYNCINE_MEMORY_VIDEO，跳过\n");
        return 0;
    }

    bool roundsOk = false;
    const int envRounds =
        qEnvironmentVariableIntValue("SYNCINE_MEMORY_ROUNDS", &roundsOk);
    const int rounds = qMax(3, roundsOk ? envRounds : 20);
    constexpr int kDecodeFrames = 30;   // 每轮至少解出这么多帧再卸载

    PlaybackController controller;
    controller.setMuted(true);          // 探针不出声

    QVideoSink sink;
    controller.bindVideoOutput(&sink);

    int frames = 0;
    QObject::connect(&sink, &QVideoSink::videoFrameChanged, &app,
                     [&frames](const QVideoFrame &frame) {
                         // clearVideoFrame() 注入的 2×2 黑帧不算解码帧
                         if (frame.isValid()
                             && !(frame.width() == 2 && frame.height() == 2))
                             ++frames;
                     });

    std::vector<qint64> loadedRss;
    std::vector<qint64> unloadedRss;

    const QUrl source = QUrl::fromLocalFile(videoPath);
    const bool rssAvailable = rssKb() >= 0;
    if (!rssAvailable)
        std::printf("media-memory: 当前平台无法读取 RSS，只检查最后一帧\n");
    std::printf("media-memory: 文件=%s 轮数=%d\n",
                qPrintable(videoPath), rounds);
    std::printf("%4s %10s %10s %8s\n", "轮", "解码后MB", "卸载后MB", "卸载ms");

    bool decodeFailed = false;
    for (int round = 1; round <= rounds; ++round) {
        frames = 0;
        controller.load(source);
        controller.play();

        const bool decoded = waitFor([&frames] { return frames >= kDecodeFrames; },
                                     20000);
        if (!decoded && !decodeFailed) {
            std::fprintf(stderr, "第 %d 轮没能解出帧（已解 %d 帧）\n",
                         round, frames);
            decodeFailed = true;
        }

        const qint64 loaded = rssKb();

        QElapsedTimer unloadTimer;
        unloadTimer.start();
        controller.unload();
        const qint64 unloadMs = unloadTimer.elapsed();
        // unload() 内部的 clearVideoFrame() 是同步的，这里再转一圈事件循环
        // 让播放器的停止信号走完。
        waitFor([] { return false; }, 100);

        const QVideoFrame lastFrame = sink.videoFrame();
        CHECK(lastFrame.isValid());
        CHECK(lastFrame.width() == 2 && lastFrame.height() == 2);

        const qint64 unloaded = rssKb();

        loadedRss.push_back(loaded);
        unloadedRss.push_back(unloaded);

        std::printf("%4d %10s %10s %8lld\n", round,
                    qPrintable(formatMb(loaded)),
                    qPrintable(formatMb(unloaded)),
                    static_cast<long long>(unloadMs));
    }

    CHECK(!decodeFailed);

    // ---- RSS 趋势 ----
    // 跳过第 1 轮：首次加载要建解码器、帧池和分配器 arena，属于"涨到平台"。
    // 之后允许波动，但不允许随轮次持续抬升。
    if (rssAvailable && loadedRss.size() > 2) {
        const qint64 loadedMin =
            *std::min_element(loadedRss.begin(), loadedRss.end());
        const qint64 loadedMax =
            *std::max_element(loadedRss.begin(), loadedRss.end());
        std::printf("media-memory: 解码后 RSS %.1f–%.1f MB\n",
                    loadedMin / 1024.0, loadedMax / 1024.0);
    }

    if (rssAvailable && unloadedRss.size() > 2) {
        const auto begin = unloadedRss.begin() + 1;
        const qint64 tailMin = *std::min_element(begin, unloadedRss.end());
        const qint64 tailMax = *std::max_element(begin, unloadedRss.end());
        const qint64 tailLast = unloadedRss.back();
        const qint64 spreadMb = (tailMax - tailMin) / 1024;
        const qint64 driftMb = (tailLast - tailMin) / 1024;

        std::printf("media-memory: 卸载后 RSS %.1f–%.1f MB（波动 %.1f MB），"
                    "末轮比最低点高 %.1f MB\n",
                    tailMin / 1024.0, tailMax / 1024.0,
                    spreadMb / 1.0, driftMb / 1.0);

        // 验收线（宽于实测波动，窄于"每轮泄漏一个 4K 帧"的量级）：
        //   · 波动带 <= 300MB
        //   · 末轮不超过最低点 250MB
        CHECK(spreadMb <= 300);
        CHECK(driftMb <= 250);
    }

    if (g_failures == 0) {
        std::printf("media-memory: 通过\n");
        return 0;
    }
    std::fprintf(stderr, "media-memory: %d 项失败\n", g_failures);
    return 1;
}
