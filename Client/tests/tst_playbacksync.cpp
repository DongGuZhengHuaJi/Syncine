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

// ============================
// 3. 屏幕共享态：入房快照、广播与模式切换
// ============================

void testScreenShareState()
{
    Fixture f;
    const QString file = makeTempFile(QStringLiteral("screen.mp4"));
    CHECK(!file.isEmpty());

    RoomSnapshot snapshot = makeSnapshot(
        QStringLiteral("room-3"), QStringLiteral("share"),
        {makeMember(QStringLiteral("client-self"), QStringLiteral("我"), true),
         makeMember(QStringLiteral("other"), QStringLiteral("B"), false)},
        makeItem(QStringLiteral("item-1")));
    snapshot.shareSource = QStringLiteral("screen");
    snapshot.shareAudio = true;

    int seekableNotifies = 0;
    QObject::connect(&f.sync, &PlaybackSync::seekableChanged, &f.room,
                     [&seekableNotifies] { ++seekableNotifies; });

    f.room.enterRoom(snapshot);
    CHECK(f.room.shareSource() == RoomSession::ShareSource::Screen);
    CHECK(f.room.shareAudio());
    CHECK(f.room.screenSharing());
    CHECK(!f.sync.seekable());
    CHECK(seekableNotifies > 0);

    // 房主停止共享：服务端广播 media
    f.network.messageReceived(
        QStringLiteral(R"({"type":"share_state","source":"media","audio":false})"));
    CHECK(f.room.shareSource() == RoomSession::ShareSource::Media);
    CHECK(!f.room.screenSharing());

    // 停止共享后进度条必须恢复可 seek（替换真实播放器的替身报告可 seek）。
    fakePlayback().seeks.clear();
    CHECK(f.sync.seekable());
    f.sync.seek(1234);
    CHECK(fakePlayback().seeks == QList<qint64>({1234}));

    // 房主本地请求立即生效（不等服务端回声），这样停止共享后进度条不会
    // 多锁一个 RTT；没有连接时发送会失败，但本地状态仍应更新。
    f.room.setShareState(RoomSession::ShareSource::Screen, false);
    CHECK(f.room.screenSharing());

    // 屏幕共享期间不允许直接切到其他视频：必须在界面上先停止共享。
    // 门禁在 RoomSession::switchTo 里（服务端只校验房主，不知道共享态）。
    QString switchError;
    QObject::connect(&f.room, &RoomSession::errorOccurred, &f.room,
                     [&switchError](const QString &message) { switchError = message; });
    f.room.switchTo(QStringLiteral("item-2"));
    CHECK(switchError.contains(QStringLiteral("停止共享")));

    f.room.setShareState(RoomSession::ShareSource::Media, false);
    CHECK(!f.room.screenSharing());
    switchError.clear();
    f.room.switchTo(QStringLiteral("item-2"));
    CHECK(switchError.isEmpty());

    // 窗口共享 + 切回本地模式：模式变化必须清掉共享源
    f.network.messageReceived(
        QStringLiteral(R"({"type":"share_state","source":"window","audio":false})"));
    CHECK(f.room.screenSharing());
    f.network.messageReceived(
        QStringLiteral(R"({"type":"room_mode_changed","mode":"local"})"));
    CHECK(f.room.shareSource() == RoomSession::ShareSource::Media);
    CHECK(!f.room.screenSharing());

    // playlistAboutToSwitch 必须早于 playlistSwitched 发出：
    // main.cpp 依赖它在 PlaybackSync 加载新条目前停止屏幕共享。
    QStringList switchOrder;
    QObject::connect(&f.room, &RoomSession::playlistAboutToSwitch, &f.room,
                     [&switchOrder](const QString &) { switchOrder << "about"; });
    QObject::connect(&f.room, &RoomSession::playlistSwitched, &f.room,
                     [&switchOrder](const QString &) { switchOrder << "switched"; });
    f.network.messageReceived(
        QStringLiteral(R"({"type":"playlist_switched","itemId":"item-1",)"
                       R"("currentIndex":0})"));
    CHECK(switchOrder == QStringList({"about", "switched"}));

    // 观众端：room_joined 随快照带 screen 时也进入共享态
    Fixture viewer;
    RoomSnapshot viewerSnapshot = makeSnapshot(
        QStringLiteral("room-4"), QStringLiteral("share"),
        {makeMember(QStringLiteral("client-self"), QStringLiteral("我"), false),
         makeMember(QStringLiteral("host"), QStringLiteral("房主"), true)},
        makeItem(QStringLiteral("item-1")));
    viewerSnapshot.shareSource = QStringLiteral("window");
    viewer.room.enterRoom(viewerSnapshot);
    CHECK(viewer.room.screenSharing());
    CHECK(viewer.room.shareSource() == RoomSession::ShareSource::Window);
    CHECK(!viewer.sync.seekable());
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
    testScreenShareState();

    if (g_failures == 0) {
        std::printf("playbacksync: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "playbacksync: %d check(s) failed\n", g_failures);
    return 1;
}
