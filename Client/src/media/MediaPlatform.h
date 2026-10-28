//
// 网链模式的平台抽象。
//
// 为什么需要：下载/探测代码里不应再出现「if (bilibili)」这类硬编码。
// URL 由注册表按 host 匹配到某个平台，平台再决定：
//   - 输入如何规范化（B 站支持 BV/av/ss/ep/直播间号，自动拼链接）
//   - 是否使用全局 cookie 文件
//   - 是否支持弹幕
//   - 需要追加的 yt-dlp 参数（为后续插件/站点特化留口）
//
// 约定：
//   - 匹配只用 QUrl::host() 的后缀，不做字符串 contains（避免 evilbilibili.com）。
//   - 显式选择的平台失败时直接报错，不静默退回 Generic。
//   - Generic 是兜底平台，接受任意 http(s) 链接。
//

#ifndef SYNCINE_MEDIAPLATFORM_H
#define SYNCINE_MEDIAPLATFORM_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <vector>

enum class CookiePolicy {
    Never,        // 绝不注入 cookie
    Always,       // 总是注入（B 站）
    IfUserEnabled // 由设置项决定（Generic）
};

class MediaPlatform {
public:
    virtual ~MediaPlatform() = default;

    virtual QString id() const = 0;
    virtual QString displayName() const = 0;
    virtual bool matches(const QUrl &url) const = 0;

    // 把用户输入规范化为完整链接。失败时返回空串并写 error。
    virtual QString normalizeInput(const QString &raw, QString *error) const = 0;

    virtual CookiePolicy cookies() const = 0;
    virtual bool supportsDanmaku() const = 0;

    virtual QStringList extraInspectArgs(const QUrl &) const { return {}; }
    virtual QStringList extraDownloadArgs(const QUrl &) const { return {}; }
};

class MediaPlatformRegistry {
public:
    MediaPlatformRegistry();

    // 返回第一个 matches() 的平台；Generic 兜底，所以对 http(s) 永不为空。
    const MediaPlatform *detect(const QUrl &url) const;
    const MediaPlatform *byId(const QString &id) const;

    // platformId 非空时只用该平台；为空时按注册表顺序逐个尝试 normalize
    // （用于通用卡片粘贴 B 站编号/链接时自动识别）。
    QString normalize(const QString &platformId, const QString &raw,
                      QString *error) const;

    QList<const MediaPlatform *> all() const;

private:
    std::vector<std::unique_ptr<MediaPlatform>> m_platforms;
};

// 进程内共享的注册表（只读）。
const MediaPlatformRegistry &mediaPlatformRegistry();

// 该平台是否应注入全局 cookie 文件。
bool platformUsesCookies(const MediaPlatform *platform, bool genericCookiesEnabled);

// QML 入口：main.cpp 以 mediaPlatforms 注入。
class PlatformBridge : public QObject {
    Q_OBJECT

public:
    explicit PlatformBridge(QObject *parent = nullptr);

    // 这个 URL 属于哪个平台 id（无法识别返回空）。
    Q_INVOKABLE QString detect(const QString &url) const;

    // { "url": 规范化后的链接, "error": 失败原因 }
    Q_INVOKABLE QVariantMap normalize(const QString &platformId,
                                      const QString &raw) const;

    // [{ id, name }]，给界面列平台卡片用。
    Q_INVOKABLE QVariantList platforms() const;

private:
    MediaPlatformRegistry m_registry;
};

#endif //SYNCINE_MEDIAPLATFORM_H
