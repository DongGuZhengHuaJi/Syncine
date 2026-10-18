//
// 网链模式的 B 站弹幕：下载 XML、解析、按播放进度取条目。
//
// 获取方式直接用 yt-dlp 的「danmaku 字幕轨」：
//   yt-dlp --skip-download --write-subs --sub-langs danmaku <pageUrl>
// B 站提取器把弹幕作为一条语言为 danmaku、格式为 xml 的字幕暴露出来
// （`--list-subs` 可见），普通视频与番剧都支持，且不需要登录。
// 这样就不必自己从媒体 URL 里抠 cid，也不用手写 view/PGC API 兜底。
//
// 只负责"数据"：QML 每 50ms 调一次 takeUntil(当前播放位置)，拿到这批该出现的
// 弹幕，自己在屏幕上做轨道分配和动画。这样 C++ 不依赖播放器几何与字体宽度。
//
// 缓存文件与下载的视频放在同一个条目目录（danmaku.xml），生命周期一致：
// 切条目只停请求不删文件，退出软件时随整个 clientId 临时目录清除。
//

#ifndef SYNCINE_DANMAKUCONTROLLER_H
#define SYNCINE_DANMAKUCONTROLLER_H

#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "danmaku/DanmakuXmlParser.h"

class CookieManager;
class Toolchain;
struct MediaInfo;

class DanmakuController : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool available
               READ available
               NOTIFY availableChanged)

    Q_PROPERTY(int count
               READ count
               NOTIFY countChanged)

public:
    // toolchain 用来定位 yt-dlp；cookies 可以为空（那就不带 --cookies）
    explicit DanmakuController(const Toolchain *toolchain,
                               const CookieManager *cookies = nullptr,
                               QObject *parent = nullptr);
    ~DanmakuController() override;

    bool available() const { return !m_items.isEmpty(); }
    int count() const { return static_cast<int>(m_items.size()); }

    // ---- PlaybackSync 调用 ----
    // 切到某一条：换目录时立即清空；同目录的请求不会被重复打断
    //（onRoomChanged 会被成员刷新反复触发，必须幂等）。
    // 返回 true 表示已经命中 danmaku.xml，可直接显示。
    bool beginItem(const QString &dir, const QString &pageUrl);
    // yt-dlp 探测成功后调用；非 B 站/直播会静默跳过。
    void loadFor(const MediaInfo &info);
    // 媒体缓存命中但没有 MediaInfo 时：直接按页面 URL 尝试下载。
    void loadFromUrl();
    void reset();

    // ---- QML 调用 ----
    // 返回 timeMs <= positionMs 的条目并推进游标；检测到 seek/大跳变时
    // 先 seekTo 并发出 jumped，由界面清屏。
    Q_INVOKABLE QVariantList takeUntil(qint64 positionMs);
    Q_INVOKABLE void seekTo(qint64 positionMs);

signals:
    void availableChanged();
    void countChanged();
    // 条目切换/清空：界面清掉时间轴上的旧弹幕
    void sourceChanged();
    // takeUntil 发现位置跳变（seek、纠偏、切集）：界面清屏
    void jumped();

private:
    enum class State {
        Idle,
        Downloading,
        Ready,
        Failed,
    };

    QString cachePath() const;
    QString tempPath(quint64 generation) const;
    bool loadCacheFile();
    void applyItems(QList<DanmakuItem> items);
    void clearItems();
    void fail(const QString &message);

    static bool isBilibiliUrl(const QString &url);
    bool isBilibili(const MediaInfo &info) const;

    void startFetch(quint64 generation);
    void handleFetchFinished(int exitCode, QProcess::ExitStatus status,
                             quint64 generation);
    void cleanupTempFiles();
    void abortFetch();

    const Toolchain *m_toolchain = nullptr;
    const CookieManager *m_cookies = nullptr;
    QPointer<QProcess> m_process;

    QString m_dir;
    QString m_pageUrl;
    QList<DanmakuItem> m_items;
    int m_cursor = 0;
    qint64 m_lastPosition = -1;
    quint64 m_generation = 0;
    State m_state = State::Idle;
    QStringList m_outputTail;
};

#endif //SYNCINE_DANMAKUCONTROLLER_H
