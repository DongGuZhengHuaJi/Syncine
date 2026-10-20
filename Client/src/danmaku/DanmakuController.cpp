#include "DanmakuController.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <utility>

#include "core/CookieManager.h"
#include "core/Log.h"
#include "core/Toolchain.h"
#include "danmaku/LiveDanmakuSession.h"
#include "media/FormatModel.h"

namespace {

constexpr int kMaxXmlBytes = 16 * 1024 * 1024;
constexpr int kMaxTailLines = 40;
// 播放器位置在正常播放时每 50ms 推进一次；超过这个值按 seek/纠偏处理。
// 4x 倍速下 50ms 也只推进约 200ms，离阈值很远。
constexpr qint64 kJumpThresholdMs = 1500;

} // namespace

DanmakuController::DanmakuController(const Toolchain *toolchain,
                                     const CookieManager *cookies,
                                     QObject *parent)
    : QObject(parent),
      m_toolchain(toolchain),
      m_cookies(cookies) {

    m_live = new LiveDanmakuSession(
        m_cookies != nullptr ? m_cookies->path() : QString(), this);
    connect(m_live, &LiveDanmakuSession::danmakuBatch,
            this, &DanmakuController::liveDanmakuBatch);
    connect(m_live, &LiveDanmakuSession::connectedChanged,
            this, &DanmakuController::liveConnectedChanged);
    connect(m_live, &LiveDanmakuSession::stopped,
            this, &DanmakuController::liveStopped);
}

DanmakuController::~DanmakuController()
{
    if (m_live != nullptr)
        m_live->stop();
    abortFetch();
}

bool DanmakuController::liveConnected() const
{
    return m_live != nullptr && m_live->connected();
}

// ============================
// PlaybackSync 入口
// ============================

bool DanmakuController::beginItem(const QString &dir, const QString &pageUrl)
{
    if (dir.isEmpty()) {
        reset();
        return false;
    }

    if (dir != m_dir) {
        // 换条目：旧请求作废，旧弹幕立刻从界面消失
        ++m_generation;
        if (m_live != nullptr)
            m_live->stop();
        abortFetch();
        clearItems();
        m_dir = dir;
        m_pageUrl = pageUrl;
        m_state = State::Idle;
        emit sourceChanged();
    } else {
        m_pageUrl = pageUrl;

        // onRoomChanged 会被成员表刷新反复触发，同一个条目的请求不能被推倒重来
        if (m_state == State::Ready)
            return true;
        if (m_state == State::Downloading)
            return false;
    }

    return loadCacheFile();
}

void DanmakuController::loadFor(const MediaInfo &info)
{
    if (m_dir.isEmpty())
        return;

    // 直播走完全不同的链路：WebSocket 实时推送，没有 XML/时间轴/游标
    if (info.live) {
        if (m_live != nullptr)
            m_live->start(info.url.isEmpty() ? m_pageUrl : info.url);
        return;
    }

    if (m_state == State::Ready || m_state == State::Downloading)
        return;
    if (!isBilibili(info)) {
        LOG_DEBUG("Danmaku") << "非 B 站源，跳过弹幕";
        return;
    }

    // yt-dlp 的 B 站提取器把弹幕暴露成语言为 danmaku 的字幕轨，
    // 不需要我们自己算 cid。媒体与弹幕各自一个 yt-dlp 进程，并行跑。
    startFetch(++m_generation);
}

void DanmakuController::loadFromUrl()
{
    if (m_dir.isEmpty())
        return;

    if (isLiveUrl(m_pageUrl)) {
        if (m_live != nullptr)
            m_live->start(m_pageUrl);
        return;
    }

    if (m_state == State::Ready || m_state == State::Downloading)
        return;
    if (!isBilibiliUrl(m_pageUrl)) {
        LOG_DEBUG("Danmaku") << "页面不是 B 站链接，跳过弹幕";
        return;
    }

    startFetch(++m_generation);
}

void DanmakuController::reset()
{
    const bool hadState = !m_dir.isEmpty() || !m_items.isEmpty()
                          || m_state != State::Idle || !m_process.isNull()
                          || (m_live != nullptr && m_live->running());
    if (!hadState)
        return;

    ++m_generation;
    if (m_live != nullptr)
        m_live->stop();
    abortFetch();
    clearItems();
    m_dir.clear();
    m_pageUrl.clear();
    m_state = State::Idle;
    emit sourceChanged();
}

// ============================
// 给 QML 的游标
// ============================

QVariantList DanmakuController::takeUntil(qint64 positionMs)
{
    if (positionMs < 0)
        positionMs = 0;

    if (m_items.isEmpty()) {
        m_lastPosition = positionMs;
        return {};
    }

    if (m_lastPosition < 0) {
        // 第一次取：从当前位置开始，不补放开头
        seekTo(positionMs);
    } else if (positionMs < m_lastPosition
               || positionMs - m_lastPosition > kJumpThresholdMs) {
        // 后退（任意幅度）或大幅前跳 = seek/纠偏/切集
        seekTo(positionMs);
        emit jumped();
    }

    QVariantList result;
    while (m_cursor < m_items.size() && m_items.at(m_cursor).timeMs <= positionMs) {
        const DanmakuItem &item = m_items.at(m_cursor);
        QVariantMap entry;
        entry.insert(QStringLiteral("time"), item.timeMs);
        entry.insert(QStringLiteral("mode"), item.mode);
        entry.insert(QStringLiteral("size"), item.fontSize);
        entry.insert(QStringLiteral("color"),
                     QStringLiteral("#%1").arg(item.color, 6, 16, QLatin1Char('0')));
        entry.insert(QStringLiteral("text"), item.text);
        result.append(entry);
        ++m_cursor;
    }

    m_lastPosition = positionMs;
    return result;
}

void DanmakuController::seekTo(qint64 positionMs)
{
    if (positionMs < 0)
        positionMs = 0;

    const auto it = std::lower_bound(
        m_items.cbegin(), m_items.cend(), positionMs,
        [](const DanmakuItem &item, qint64 position) {
            return item.timeMs < position;
        });
    m_cursor = static_cast<int>(it - m_items.cbegin());
    m_lastPosition = positionMs;
}

// ============================
// 缓存与状态
// ============================

QString DanmakuController::cachePath() const
{
    if (m_dir.isEmpty())
        return {};
    return m_dir + QStringLiteral("/danmaku.xml");
}

QString DanmakuController::tempPath(quint64 generation) const
{
    if (m_dir.isEmpty())
        return {};
    // yt-dlp 的字幕文件名是 <输出基名>.<语言>.<扩展名>:
    //   -o ".../danmaku-dl.<gen>.%(ext)s" → danmaku-dl.<gen>.danmaku.xml
    return m_dir + QStringLiteral("/danmaku-dl.") + QString::number(generation)
           + QStringLiteral(".danmaku.xml");
}

bool DanmakuController::loadCacheFile()
{
    const QString path = cachePath();
    if (path.isEmpty())
        return false;

    QFile file(path);
    if (!file.exists())
        return false;
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_WARN("Danmaku") << "弹幕缓存无法打开:" << path << file.errorString();
        return false;
    }

    const QByteArray data = file.readAll();
    if (data.size() > kMaxXmlBytes) {
        LOG_WARN("Danmaku") << "弹幕缓存过大，忽略:" << data.size();
        return false;
    }

    const DanmakuParseResult parsed = DanmakuXmlParser::parse(data);
    if (!parsed.ok) {
        LOG_WARN("Danmaku") << "弹幕缓存解析失败:" << path << parsed.error;
        return false;
    }

    applyItems(parsed.items);
    m_state = State::Ready;
    LOG_INFO("Danmaku") << "弹幕缓存命中:" << path << "条数=" << count();
    return true;
}

void DanmakuController::applyItems(QList<DanmakuItem> items)
{
    const bool wasAvailable = available();
    const int oldCount = count();

    m_items = std::move(items);
    m_cursor = 0;
    m_lastPosition = -1;

    const bool nowAvailable = available();
    if (wasAvailable != nowAvailable)
        emit availableChanged();
    if (oldCount != count())
        emit countChanged();
}

void DanmakuController::clearItems()
{
    const bool wasAvailable = available();
    const int oldCount = count();

    m_items.clear();
    m_cursor = 0;
    m_lastPosition = -1;

    if (wasAvailable)
        emit availableChanged();
    if (oldCount != 0)
        emit countChanged();
}

void DanmakuController::fail(const QString &message)
{
    LOG_WARN("Danmaku") << message;
    m_state = State::Failed;
}

// ============================
// 下载：yt-dlp 的 danmaku 字幕轨
// ============================

bool DanmakuController::isBilibiliUrl(const QString &url)
{
    return url.contains(QLatin1String("bilibili.com"), Qt::CaseInsensitive)
           || url.contains(QLatin1String("b23.tv"), Qt::CaseInsensitive);
}

bool DanmakuController::isLiveUrl(const QString &url)
{
    return url.contains(QLatin1String("live.bilibili.com"), Qt::CaseInsensitive);
}

bool DanmakuController::isBilibili(const MediaInfo &info) const
{
    return info.extractor.contains(QLatin1String("bilibili"), Qt::CaseInsensitive)
           || isBilibiliUrl(info.url)
           || isBilibiliUrl(m_pageUrl);
}

void DanmakuController::cleanupTempFiles()
{
    const QDir dir(m_dir);
    const QStringList stale =
        dir.entryList({QStringLiteral("danmaku-dl.*.danmaku.xml")}, QDir::Files);
    for (const QString &name : stale)
        QFile::remove(dir.absoluteFilePath(name));
}

void DanmakuController::startFetch(quint64 generation)
{
    if (m_toolchain == nullptr || !m_toolchain->ytDlpReady()) {
        fail(QStringLiteral("找不到 yt-dlp，无法下载弹幕"));
        return;
    }
    if (!Toolchain::isSafeMediaUrl(m_pageUrl)) {
        fail(QStringLiteral("条目地址不是有效的 http/https 链接，已跳过弹幕"));
        return;
    }

    QDir().mkpath(m_dir);
    cleanupTempFiles();

    m_state = State::Downloading;
    m_outputTail.clear();

    // 临时文件名带 generation：被取消的旧进程即使晚一步写完，
    // 也不会覆盖新请求的文件。
    const QString outputTemplate =
        m_dir + QStringLiteral("/danmaku-dl.") + QString::number(generation)
        + QStringLiteral(".%(ext)s");

    QStringList args{
        QStringLiteral("--skip-download"),
        QStringLiteral("--write-subs"),
        QStringLiteral("--sub-langs"), QStringLiteral("danmaku"),
        QStringLiteral("--no-playlist"),
        QStringLiteral("--newline"),
        QStringLiteral("-o"), outputTemplate
    };
    args << m_toolchain->ffmpegLocationArgs();
    if (m_cookies != nullptr)
        args << m_cookies->cookieArgs();
    Toolchain::appendUrlArg(args, m_pageUrl);

    LOG_INFO("Danmaku") << "下载弹幕:" << m_pageUrl;

    QProcess *process = new QProcess(this);
    m_process = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setProgram(m_toolchain->ytDlpPath());
    process->setArguments(args);

    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, process, generation]() {
                const QString chunk =
                    QString::fromUtf8(process->readAllStandardOutput());
                if (generation != m_generation)
                    return;
                const QStringList lines =
                    chunk.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                for (const QString &line : lines) {
                    const QString trimmed = line.trimmed();
                    if (trimmed.isEmpty())
                        continue;
                    m_outputTail.append(trimmed);
                    while (m_outputTail.size() > kMaxTailLines)
                        m_outputTail.removeFirst();
                }
            });

    connect(process, &QProcess::errorOccurred, this,
            [this, process, generation](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;

                const QString errorText = process->errorString();
                if (process == m_process)
                    m_process = nullptr;
                process->deleteLater();
                QFile::remove(tempPath(generation));

                if (generation != m_generation)
                    return;
                fail(QStringLiteral("yt-dlp 启动失败: %1").arg(errorText));
            });

    connect(process, &QProcess::finished, this,
            [this, process, generation](int exitCode, QProcess::ExitStatus status) {
                if (process == m_process)
                    m_process = nullptr;
                process->deleteLater();
                handleFetchFinished(exitCode, status, generation);
            });

    process->start();
}

void DanmakuController::handleFetchFinished(int exitCode,
                                            QProcess::ExitStatus status,
                                            quint64 generation)
{
    const QString temp = tempPath(generation);

    // 旧请求（条目已切走/重置）：只清掉自己的临时文件
    if (generation != m_generation || m_state != State::Downloading) {
        QFile::remove(temp);
        return;
    }

    QFile file(temp);
    if (exitCode != 0 || status != QProcess::NormalExit || !file.exists()) {
        QString detail;
        if (!m_outputTail.isEmpty())
            detail = m_outputTail.last();
        fail(QStringLiteral("弹幕下载失败（yt-dlp 退出码 %1）%2")
                 .arg(exitCode)
                 .arg(detail.isEmpty() ? QString()
                                       : QStringLiteral("：") + detail));
        QFile::remove(temp);
        return;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("弹幕文件打不开: %1").arg(file.errorString()));
        QFile::remove(temp);
        return;
    }
    const QByteArray data = file.readAll();
    file.close();

    if (data.size() > kMaxXmlBytes) {
        fail(QStringLiteral("弹幕 XML 过大: %1 字节").arg(data.size()));
        QFile::remove(temp);
        return;
    }

    DanmakuParseResult parsed = DanmakuXmlParser::parse(data);
    if (!parsed.ok) {
        fail(QStringLiteral("弹幕 XML 解析失败: %1").arg(parsed.error));
        QFile::remove(temp);
        return;
    }

    // 解析成功才写正式缓存（包括确实 0 条的合法 XML），下次切回来直接命中
    QSaveFile cache(cachePath());
    if (!cache.open(QIODevice::WriteOnly)) {
        LOG_WARN("Danmaku") << "弹幕缓存打不开:" << cache.errorString();
    } else {
        cache.write(data);
        if (!cache.commit())
            LOG_WARN("Danmaku") << "弹幕缓存写入失败:" << cache.errorString();
    }

    QFile::remove(temp);

    applyItems(std::move(parsed.items));
    m_state = State::Ready;
    LOG_INFO("Danmaku") << "弹幕解析完成: 保留" << count() << "条, 总数"
                        << parsed.total << ", 过滤" << parsed.skipped
                        << (parsed.truncated ? "（达到上限）" : "");
}

void DanmakuController::abortFetch()
{
    if (m_process.isNull())
        return;

    QProcess *process = m_process.data();
    m_process = nullptr;
    process->kill();
    // 切条目/离房时最多等半秒，确保进程真的退出：
    // 否则 QProcess 析构时会报 "Destroyed while process is still running"，
    // 而且临时文件可能还在写。SIGKILL 通常几毫秒就结束。
    process->waitForFinished(500);
    // finished 回调会负责 deleteLater，并按 generation 清理临时文件
}
