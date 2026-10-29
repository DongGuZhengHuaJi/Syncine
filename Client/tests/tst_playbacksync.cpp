//
// PlaybackSync 的加载时机回归测试。
//
// 验证两件与内存相关的事：
//   1. 成员表刷新（加入/离开/video_status 回显）不得触碰播放器；
//      只有房主身份变化才重新决定"本地解码还是看远端"。
//   2. "已就绪"标记绑定实际播放源：本地/网链模式往返后不能因为条目 id
//      相同就误判为已准备好、跳过重新解析。
//
// 房间、协议编解码、成员/播放列表模型都是真实实现；消息按服务端格式从
// NetworkManager::messageReceived 注入。播放器用 tests/fake_playback.cpp
// 的替身（记录每次 load/unload），不真的解码媒体。
//

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

#include "core/NetworkManager.h"
#include "core/RoomTypes.h"
#include "core/Toolchain.h"
#include "core/UrlDownloader.h"
#include "danmaku/DanmakuController.h"
#include "playback/PlaybackController.h"
#include "playback/PlaybackSync.h"
#include "session/RoomSession.h"

#include "fake_playback.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                        \
        }                                                                        \
    } while (0)

QTemporaryDir *g_tempDir = nullptr;

QString makeTempFile(const QString &name)
{
    const QString path = g_tempDir->filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write("test");
    return path;
}

Member makeMember(const QString &id, const QString &nickname, bool host)
{
    Member member;
    member.clientId = id;
    member.nickname = nickname;
    member.isHost = host;
    return member;
}

PlaylistEntry makeItem(const QString &id, const QString &url = {})
{
    PlaylistEntry entry;
    entry.itemId = id;
    entry.title = id;
    entry.url = url;
    return entry;
}

// 一套真实对象。PlaybackSync 在最后构造（它要连接前面所有对象）。
struct Fixture {
    PlaybackController playback;
    NetworkManager network{nullptr};
    RoomSession room{&network};
    Toolchain toolchain;
    UrlDownloader downloader{&toolchain};
    DanmakuController danmaku{&toolchain};
    PlaybackSync sync{&room, &playback, &downloader, &danmaku};
};

RoomSnapshot makeSnapshot(const QString &roomId, const QString &mode,
                          const QList<Member> &members, const PlaylistEntry &item)
{
    RoomSnapshot snapshot;
    snapshot.roomId = roomId;
    snapshot.roomName = roomId;
    snapshot.mode = mode;
    snapshot.clientId = QStringLiteral("client-self");
    snapshot.members = members;
    snapshot.playlist = {item};
    snapshot.currentIndex = 0;
    return snapshot;
}

// ============================
// 1. 成员表刷新不重建媒体
// ============================

void testMemberRefreshDoesNotRebuildMedia()
{
    Fixture f;
    const QString fileA = makeTempFile(QStringLiteral("a.mp4"));
    CHECK(!fileA.isEmpty());

    // 记录房间状态刷新的次数：下面每一步都先确认 membersChanged 真的发生了，
    // 否则"没有触碰播放器"可能只是消息被忽略造成的假通过。
    int memberRefreshes = 0;
    QObject::connect(&f.room, &RoomSession::membersChanged, &f.room,
                     [&memberRefreshes] { ++memberRefreshes; });

    f.room.enterRoom(makeSnapshot(
        QStringLiteral("room-1"), QStringLiteral("share"),
        {makeMember(QStringLiteral("client-self"), QStringLiteral("我"), true)},
        makeItem(QStringLiteral("item-1"))));

    // 共享模式房主：加载本机文件
    f.room.assignLocalFile(QStringLiteral("item-1"),
                           QUrl::fromLocalFile(fileA));
    CHECK(fakePlayback().source == QUrl::fromLocalFile(fileA));

    fakePlayback().clear();

    // ---- 普通成员加入 ----
    memberRefreshes = 0;
    f.network.messageReceived(
        QStringLiteral(R"({"type":"member_joined","clientId":"other",)"
                       R"("nickname":"B"})"));
    CHECK(memberRefreshes > 0);
    CHECK(fakePlayback().loads.isEmpty());
    CHECK(fakePlayback().unloads == 0);
    CHECK(fakePlayback().source == QUrl::fromLocalFile(fileA));

    // ---- video_status 回显（服务端把成员状态广播回来） ----
    memberRefreshes = 0;
    f.network.messageReceived(
        QStringLiteral(R"({"type":"video_status","clientId":"other",)"
                       R"("loaded":true,"duration":120000,)"
                       R"("itemId":"item-1","seq":1})"));
    CHECK(memberRefreshes > 0);
    CHECK(fakePlayback().loads.isEmpty());
    CHECK(fakePlayback().unloads == 0);

    // ---- 成员离开 ----
    memberRefreshes = 0;
    f.network.messageReceived(
        QStringLiteral(R"({"type":"member_left","clientId":"other"})"));
    CHECK(memberRefreshes > 0);
    CHECK(fakePlayback().loads.isEmpty());
    CHECK(fakePlayback().unloads == 0);

    // ---- 房主身份变化：自己不再是房主，共享模式下应改看远端 ----
    // （成员快照整体替换，等价于服务端重发名单；直接走 MemberModel 的变更路径）
    memberRefreshes = 0;
    f.room.members()->reset({
        makeMember(QStringLiteral("client-self"), QStringLiteral("我"), false),
        makeMember(QStringLiteral("other"), QStringLiteral("B"), true),
    });
    CHECK(memberRefreshes > 0);
    CHECK(fakePlayback().unloads == 1);
    CHECK(fakePlayback().loads.isEmpty());
    CHECK(!fakePlayback().source.isValid());

    // ---- 重新成为房主：恢复本地解码 ----
    memberRefreshes = 0;
    f.room.members()->reset({
        makeMember(QStringLiteral("client-self"), QStringLiteral("我"), true),
        makeMember(QStringLiteral("other"), QStringLiteral("B"), false),
    });
    CHECK(memberRefreshes > 0);
    CHECK(fakePlayback().loads.size() == 1);
    CHECK(fakePlayback().loads.value(0) == QUrl::fromLocalFile(fileA));
    CHECK(fakePlayback().unloads == 1);

    // 重新成为房主之后，成员刷新仍然不能碰播放器
    fakePlayback().clear();
    f.network.messageReceived(
        QStringLiteral(R"({"type":"video_status","clientId":"other",)"
                       R"("loaded":false,"duration":0,)"
                       R"("itemId":"item-1","seq":2})"));
    CHECK(fakePlayback().loads.isEmpty());
    CHECK(fakePlayback().unloads == 0);
}

// ============================
// 2. "已就绪"绑定实际播放源
// ============================

void testPreparedSourceBoundToActualSource()
{
    Fixture f;
    const QString cachePath = makeTempFile(QStringLiteral("cache.mp4"));
    const QString localPath = makeTempFile(QStringLiteral("local.mp4"));
    CHECK(!cachePath.isEmpty() && !localPath.isEmpty());

    // 网链模式下已下载好的缓存：生产里由 onUrlReady 记在条目上
    f.room.playlist()->setLocalFile(QStringLiteral("url-item"), cachePath);

    f.room.enterRoom(makeSnapshot(
        QStringLiteral("room-2"), QStringLiteral("url"),
        {makeMember(QStringLiteral("client-self"), QStringLiteral("我"), true)},
        makeItem(QStringLiteral("url-item"),
                 QStringLiteral("https://example.com/video"))));

    // 有缓存：直接加载缓存文件
    CHECK(fakePlayback().loads.size() == 1);
    CHECK(fakePlayback().loads.value(0) == QUrl::fromLocalFile(cachePath));

    // 切到本地模式（服务端广播），再给同一条目换一个本机文件
    f.network.messageReceived(
        QStringLiteral(R"({"type":"room_mode_changed","mode":"local"})"));
    f.room.assignLocalFile(QStringLiteral("url-item"),
                           QUrl::fromLocalFile(localPath));
    CHECK(fakePlayback().source == QUrl::fromLocalFile(localPath));

    fakePlayback().clear();

    // 切回网链模式：条目 id 没变，但播放器里是本地文件，不能当成"已就绪"。
    // 注意 PlaylistModel 目前只有一张"本机文件"映射，网链缓存与本机文件共用
    // 它，所以这一轮最终加载的仍可能是本地文件；这里断言的是**没有再被
    // 旧的 prepared 标记短路**（修复前 load() 一次都不会发生）。
    f.network.messageReceived(
        QStringLiteral(R"({"type":"room_mode_changed","mode":"url"})"));
    CHECK(!fakePlayback().loads.isEmpty());
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        std::fprintf(stderr, "无法创建临时目录\n");
        return 1;
    }
    g_tempDir = &tempDir;

    testMemberRefreshDoesNotRebuildMedia();
    testPreparedSourceBoundToActualSource();

    if (g_failures == 0) {
        std::printf("playbacksync: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "playbacksync: %d check(s) failed\n", g_failures);
    return 1;
}
