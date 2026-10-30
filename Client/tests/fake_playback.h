//
// PlaybackController 的测试替身（见 fake_playback.cpp）。
//
// PlaybackSync 的加载时机测试不需要真的解码视频，只需要知道它**有没有**
// 去动播放器。真实 load() 对同源调用会静默返回，那会把多余的调用藏起来；
// 替身因此记录每一次 load()/unload() 调用，断言"成员表刷新不得触碰播放器"
// 时看调用次数，比看 sourceChanged 严格。
//
// 整个测试进程只创建一个 PlaybackController，状态放在这里。
//

#ifndef SYNCINE_TESTS_FAKE_PLAYBACK_H
#define SYNCINE_TESTS_FAKE_PLAYBACK_H

#include <QList>
#include <QUrl>

struct FakePlaybackLog {
    // 每一次 load()（含与当前源相同的调用）
    QList<QUrl> loads;
    // unload() 次数
    int unloads = 0;
    // 最近一次 load/unload 之后播放器的源；无效 = 未加载
    QUrl source;
    bool showingRemote = false;
    // 替身默认报告可 seek；测试可据此验证共享门控解除后进度条恢复。
    bool seekable = true;
    QList<qint64> seeks;

    // 只清调用记录，不动 source（source 是播放器状态，不是调用日志）
    void clear();
};

FakePlaybackLog &fakePlayback();

#endif //SYNCINE_TESTS_FAKE_PLAYBACK_H
