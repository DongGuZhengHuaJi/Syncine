#include "media/MediaPlatform.h"

#include <QRegularExpression>

namespace {

bool hostIsOrSubdomain(const QUrl &url, const QString &domain)
{
    const QString host = url.host().toLower();
    return host == domain || host.endsWith(QLatin1Char('.') + domain);
}

bool isHttpUrl(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    return (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
           && !url.host().isEmpty();
}

// 规范化完整链接：https、host 小写、去掉 fragment 和路径末尾的 /。
// 保留 query（B 站的 ?p=2 是分 P 参数）。
QString canonicalize(QUrl url)
{
    url.setScheme(QStringLiteral("https"));
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1)
        path.chop(1);
    url.setPath(path);
    url.setFragment(QString());
    return url.toString(QUrl::FullyEncoded);
}

bool startsWithScheme(const QString &text)
{
    return text.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
           || text.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
}

// ---------------------------------------------------------------------------

class BilibiliPlatform final : public MediaPlatform {
public:
    QString id() const override { return QStringLiteral("bilibili"); }
    QString displayName() const override { return QStringLiteral("Bilibili"); }

    bool matches(const QUrl &url) const override
    {
        if (!isHttpUrl(url))
            return false;
        return hostIsOrSubdomain(url, QStringLiteral("bilibili.com"))
               || hostIsOrSubdomain(url, QStringLiteral("b23.tv"));
    }

    QString normalizeInput(const QString &raw, QString *error) const override
    {
        const QString text = raw.trimmed();
        if (text.isEmpty()) {
            if (error) *error = QStringLiteral("请输入 B 站链接或编号");
            return {};
        }

        // 规则见 docs/bilibili_url.txt。
        static const QRegularExpression bv(
            QStringLiteral("^[Bb][Vv][0-9A-Za-z]{10}$"));
        static const QRegularExpression av(
            QStringLiteral("^[Aa][Vv](\\d+)$"));
        static const QRegularExpression ss(
            QStringLiteral("^[Ss][Ss](\\d+)$"));
        static const QRegularExpression ep(
            QStringLiteral("^[Ee][Pp](\\d+)$"));
        static const QRegularExpression room(QStringLiteral("^\\d+$"));

        QRegularExpressionMatch match = bv.match(text);
        if (match.hasMatch()) {
            return QStringLiteral("https://www.bilibili.com/video/BV")
                   + text.mid(2);
        }

        match = av.match(text);
        if (match.hasMatch())
            return QStringLiteral("https://www.bilibili.com/video/av") + match.captured(1);

        match = ss.match(text);
        if (match.hasMatch())
            return QStringLiteral("https://www.bilibili.com/bangumi/play/ss")
                   + match.captured(1);

        match = ep.match(text);
        if (match.hasMatch())
            return QStringLiteral("https://www.bilibili.com/bangumi/play/ep")
                   + match.captured(1);

        // 纯数字按直播间号处理；番剧请加 ss/ep 前缀。
        if (room.match(text).hasMatch())
            return QStringLiteral("https://live.bilibili.com/") + text;

        QString withScheme = text;
        if (!startsWithScheme(withScheme)) {
            const QString lower = withScheme.toLower();
            const bool looksLikeHost =
                lower.startsWith(QLatin1String("bilibili.com"))
                || lower.startsWith(QLatin1String("www.bilibili.com"))
                || lower.startsWith(QLatin1String("live.bilibili.com"))
                || lower.startsWith(QLatin1String("b23.tv"))
                || lower.startsWith(QLatin1String("www.b23.tv"));
            if (!looksLikeHost) {
                if (error) {
                    *error = QStringLiteral(
                        "无法识别的 B 站输入；支持 BV号 / av号 / ss号 / ep号 / "
                        "直播间号或完整链接");
                }
                return {};
            }
            withScheme.prepend(QStringLiteral("https://"));
        }

        QUrl url(withScheme);
        if (!url.isValid() || !matches(url)) {
            if (error) *error = QStringLiteral("这不是有效的 B 站链接");
            return {};
        }
        // 裸 bilibili.com 统一成 www，保证与完整链接得到同一个 itemId。
        if (url.host().compare(QLatin1String("bilibili.com"), Qt::CaseInsensitive) == 0)
            url.setHost(QStringLiteral("www.bilibili.com"));
        return canonicalize(url);
    }

    CookiePolicy cookies() const override { return CookiePolicy::Always; }
    bool supportsDanmaku() const override { return true; }
};

// ---------------------------------------------------------------------------

class GenericPlatform final : public MediaPlatform {
public:
    QString id() const override { return QStringLiteral("generic"); }
    QString displayName() const override { return QStringLiteral("通用链接"); }

    bool matches(const QUrl &url) const override { return isHttpUrl(url); }

    QString normalizeInput(const QString &raw, QString *error) const override
    {
        const QString text = raw.trimmed();
        if (text.isEmpty()) {
            if (error) *error = QStringLiteral("请粘贴完整视频链接");
            return {};
        }

        if (startsWithScheme(text)) {
            const QUrl url(text);
            if (!url.isValid() || !isHttpUrl(url)) {
                if (error) *error = QStringLiteral("这不是有效的 http(s) 链接");
                return {};
            }
            return url.toString(QUrl::FullyEncoded);
        }

        // 允许省略 scheme 的常见域名，避免把「www.example.com/x」误判为编号。
        static const QRegularExpression domain(
            QStringLiteral("^[A-Za-z0-9.-]+\\.[A-Za-z]{2,}(:\\d+)?(/.*)?$"));
        if (domain.match(text).hasMatch()) {
            const QUrl url(QStringLiteral("https://") + text);
            if (url.isValid() && isHttpUrl(url))
                return url.toString(QUrl::FullyEncoded);
        }

        if (error) {
            *error = QStringLiteral(
                "自定义链接只接受完整链接；B 站编号请使用 Bilibili 来源");
        }
        return {};
    }

    CookiePolicy cookies() const override { return CookiePolicy::IfUserEnabled; }
    bool supportsDanmaku() const override { return false; }
};

} // namespace

// ---------------------------------------------------------------------------

MediaPlatformRegistry::MediaPlatformRegistry()
{
    // 顺序有意义：Bilibili 先匹配，Generic 永远兜底。
    m_platforms.push_back(std::make_unique<BilibiliPlatform>());
    m_platforms.push_back(std::make_unique<GenericPlatform>());
}

const MediaPlatform *MediaPlatformRegistry::detect(const QUrl &url) const
{
    if (!url.isValid())
        return nullptr;
    for (const auto &platform : m_platforms) {
        if (platform->matches(url))
            return platform.get();
    }
    return nullptr;
}

const MediaPlatform *MediaPlatformRegistry::byId(const QString &id) const
{
    for (const auto &platform : m_platforms) {
        if (platform->id() == id)
            return platform.get();
    }
    return nullptr;
}

QString MediaPlatformRegistry::normalize(const QString &platformId,
                                         const QString &raw, QString *error) const
{
    if (!platformId.isEmpty()) {
        const MediaPlatform *platform = byId(platformId);
        if (platform == nullptr) {
            if (error) *error = QStringLiteral("未知的视频来源");
            return {};
        }
        return platform->normalizeInput(raw, error);
    }

    // 没指定来源：逐个尝试；Bilibili 的编号规则先命中，URL 最终落到 Generic。
    QString lastError;
    for (const auto &platform : m_platforms) {
        QString platformError;
        const QString normalized = platform->normalizeInput(raw, &platformError);
        if (!normalized.isEmpty())
            return normalized;
        if (!platformError.isEmpty())
            lastError = platformError;
    }
    if (error)
        *error = lastError.isEmpty() ? QStringLiteral("无法识别这个输入") : lastError;
    return {};
}

QList<const MediaPlatform *> MediaPlatformRegistry::all() const
{
    QList<const MediaPlatform *> result;
    result.reserve(static_cast<int>(m_platforms.size()));
    for (const auto &platform : m_platforms)
        result.append(platform.get());
    return result;
}

const MediaPlatformRegistry &mediaPlatformRegistry()
{
    static const MediaPlatformRegistry registry;
    return registry;
}

bool platformUsesCookies(const MediaPlatform *platform, bool genericCookiesEnabled)
{
    if (platform == nullptr)
        return false;
    switch (platform->cookies()) {
    case CookiePolicy::Always:
        return true;
    case CookiePolicy::IfUserEnabled:
        return genericCookiesEnabled;
    case CookiePolicy::Never:
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------

PlatformBridge::PlatformBridge(QObject *parent)
    : QObject(parent)
{
}

QString PlatformBridge::detect(const QString &url) const
{
    const QUrl parsed(url);
    if (!parsed.isValid())
        return {};
    const MediaPlatform *platform = m_registry.detect(parsed);
    return platform != nullptr ? platform->id() : QString();
}

QVariantMap PlatformBridge::normalize(const QString &platformId,
                                      const QString &raw) const
{
    QString error;
    const QString url = m_registry.normalize(platformId, raw, &error);
    QVariantMap result;
    result.insert(QStringLiteral("url"), url);
    result.insert(QStringLiteral("error"), error);
    return result;
}

QVariantList PlatformBridge::platforms() const
{
    QVariantList result;
    for (const MediaPlatform *platform : m_registry.all()) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), platform->id());
        row.insert(QStringLiteral("name"), platform->displayName());
        result.append(row);
    }
    return result;
}
