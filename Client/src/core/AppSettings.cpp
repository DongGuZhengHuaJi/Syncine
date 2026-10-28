#include "AppSettings.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

#include "core/Log.h"

namespace {

// 兜底服务器：用户没在设置里填地址时连它。
//
// 改地址就改这一行（客户端允许 ws/wss 两种 scheme）。1.0 发的是明文 ws://，
// 即信令（房间口令、聊天、服务端下发的 TURN 临时凭据）在网上可被中间人读到；
// 媒体面走 WebRTC 自带加密，不受影响。将来有域名了在这里改成 wss:// 即可。
const char *kFallbackServerUrl = "ws://8.148.12.233:8765";

const char *kKeyServerUrl = "server/userUrl";
const char *kKeyQuality = "playback/quality";
const char *kKeyHdrAllowed = "playback/hdrAllowed";
const char *kKeyGenericCookies = "cookies/useForGeneric";
const char *kKeyDanmakuArea = "danmaku/area";
const char *kKeyDanmakuOpacity = "danmaku/opacity";
const char *kKeyDanmakuFontSize = "danmaku/fontSize";
const char *kKeyDanmakuSpeed = "danmaku/speed";
const char *kKeyDanmakuScaleWithScreen = "danmaku/scaleWithScreen";
const char *kKeyMicDevice = "audio/micDevice";
const char *kKeySpeakerDevice = "audio/speakerDevice";
const char *kKeyLogLevel = "log/level";
const char *kKeyLastCookieBrowser = "cookies/lastBrowser";
const char *kKeyCookieSource = "cookies/source";
const char *kKeyCookieImportedAt = "cookies/importedAt";

Log::Level levelFromName(const QString &name)
{
    const QString text = name.trimmed().toLower();
    if (text == QLatin1String("trace"))
        return Log::Level::Trace;
    if (text == QLatin1String("debug"))
        return Log::Level::Debug;
    if (text == QLatin1String("warn"))
        return Log::Level::Warn;
    if (text == QLatin1String("error"))
        return Log::Level::Error;
    return Log::Level::Info;
}

} // namespace

AppSettings::AppSettings(QObject *parent)
    : QObject(parent) {

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!dir.isEmpty())
        QDir().mkpath(dir);

    m_settings = std::make_unique<QSettings>(dir + QStringLiteral("/syncine.ini"),
                                             QSettings::IniFormat);

    // 读取设置
    m_serverUrl = m_settings->value(QLatin1String(kKeyServerUrl)).toString();
    m_lastCookieBrowser = m_settings->value(QLatin1String(kKeyLastCookieBrowser)).toString();
    m_cookieSource = m_settings->value(QLatin1String(kKeyCookieSource)).toString();
    m_cookieImportedAt = m_settings->value(QLatin1String(kKeyCookieImportedAt)).toString();
    // 旧版本此处存 int（高度上限，0 = 最高），现为字符串，读取兼容两种写法：
    // "0" 归一化为空串（两者语义相同，均为不设限）
    m_defaultQuality = m_settings->value(QLatin1String(kKeyQuality)).toString().trimmed();
    if (m_defaultQuality == QLatin1String("0"))
        m_defaultQuality.clear();
    m_hdrAllowed = m_settings->value(QLatin1String(kKeyHdrAllowed), false).toBool();
    m_genericCookies = m_settings->value(QLatin1String(kKeyGenericCookies), false).toBool();
    m_danmakuArea = qBound(10, m_settings->value(QLatin1String(kKeyDanmakuArea), 100).toInt(), 100);
    m_danmakuOpacity = qBound(10, m_settings->value(QLatin1String(kKeyDanmakuOpacity), 100).toInt(), 100);
    m_danmakuFontSize = qBound(50, m_settings->value(QLatin1String(kKeyDanmakuFontSize), 100).toInt(), 200);
    m_danmakuSpeed = qBound(0, m_settings->value(QLatin1String(kKeyDanmakuSpeed), 2).toInt(), 4);
    m_danmakuScaleWithScreen = m_settings->value(QLatin1String(kKeyDanmakuScaleWithScreen), true).toBool();
    m_micDevice = m_settings->value(QLatin1String(kKeyMicDevice)).toString();
    m_speakerDevice = m_settings->value(QLatin1String(kKeySpeakerDevice)).toString();
    m_logLevel = m_settings->value(QLatin1String(kKeyLogLevel),
                                   QStringLiteral("info")).toString().toLower();

    LOG_INFO("Settings") << "配置文件:" << filePath()
                         << " 服务器地址:" << (m_serverUrl.isEmpty() ? "(兜底)" : m_serverUrl)
                         << " 画质:" << (m_defaultQuality.isEmpty() ? "(不设限)" : m_defaultQuality)
                         << " HDR:" << (m_hdrAllowed ? 1 : 0)
                         << " 日志级别:" << m_logLevel;
}

QString AppSettings::fallbackServerUrl() const
{
    return QString::fromLatin1(kFallbackServerUrl);
}

QString AppSettings::filePath() const
{
    return m_settings != nullptr ? m_settings->fileName() : QString();
}

QString AppSettings::logDirectory() const
{
    // Log 的文件名按天划分（syncine-2026-10-12.log），此处需要的是其所在目录
    const QString file = Log::filePath();
    return file.isEmpty() ? QString() : QFileInfo(file).absolutePath();
}

void AppSettings::openLogDirectory()
{
    const QString dir = logDirectory();
    if (dir.isEmpty()) {
        LOG_WARN("Settings") << "日志目录不可用，无法打开";
        return;
    }

    LOG_INFO("Settings") << "打开日志目录:" << dir;
    // 交给桌面环境用默认文件管理器打开；打不开不影响程序，无需处理返回值
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void AppSettings::setServerUrl(const QString &serverUrl)
{
    const QString trimmed = serverUrl.trimmed();
    if (m_serverUrl == trimmed)
        return;

    m_serverUrl = trimmed;
    m_settings->setValue(QLatin1String(kKeyServerUrl), m_serverUrl);
    emit serverUrlChanged();
}

void AppSettings::setLastCookieBrowser(const QString &key)
{
    if (m_lastCookieBrowser == key)
        return;

    m_lastCookieBrowser = key;
    m_settings->setValue(QLatin1String(kKeyLastCookieBrowser), m_lastCookieBrowser);
    emit lastCookieBrowserChanged();
}

void AppSettings::setCookieSource(const QString &source)
{
    if (m_cookieSource == source)
        return;

    m_cookieSource = source;
    m_settings->setValue(QLatin1String(kKeyCookieSource), m_cookieSource);
    emit cookieSourceChanged();
}

void AppSettings::setCookieImportedAt(const QString &isoTime)
{
    if (m_cookieImportedAt == isoTime)
        return;

    // 存 ISO 字符串而非 QDateTime：ini 中本就是纯文本，跨版本不会变格式
    m_cookieImportedAt = isoTime;
    m_settings->setValue(QLatin1String(kKeyCookieImportedAt), m_cookieImportedAt);
    emit cookieImportedAtChanged();
}

void AppSettings::setDefaultQuality(const QString &quality)
{
    // "0" 与空串语义相同（均为不设限），统一归一化为空串，避免界面取值反复切换
    const QString normalized = quality == QLatin1String("0") ? QString() : quality;
    if (m_defaultQuality == normalized)
        return;

    m_defaultQuality = normalized;
    m_settings->setValue(QLatin1String(kKeyQuality), m_defaultQuality);
    emit defaultQualityChanged();
}

void AppSettings::setHdrAllowed(bool allowed)
{
    if (m_hdrAllowed == allowed)
        return;

    m_hdrAllowed = allowed;
    m_settings->setValue(QLatin1String(kKeyHdrAllowed), m_hdrAllowed);
    emit hdrAllowedChanged();
}

void AppSettings::setGenericCookies(bool enabled)
{
    if (m_genericCookies == enabled)
        return;

    m_genericCookies = enabled;
    m_settings->setValue(QLatin1String(kKeyGenericCookies), m_genericCookies);
    emit genericCookiesChanged();
}

void AppSettings::setDanmakuArea(int area)
{
    area = qBound(10, area, 100);
    if (m_danmakuArea == area)
        return;

    m_danmakuArea = area;
    m_settings->setValue(QLatin1String(kKeyDanmakuArea), m_danmakuArea);
    emit danmakuAreaChanged();
}

void AppSettings::setDanmakuOpacity(int opacity)
{
    opacity = qBound(10, opacity, 100);
    if (m_danmakuOpacity == opacity)
        return;

    m_danmakuOpacity = opacity;
    m_settings->setValue(QLatin1String(kKeyDanmakuOpacity), m_danmakuOpacity);
    emit danmakuOpacityChanged();
}

void AppSettings::setDanmakuFontSize(int percent)
{
    percent = qBound(50, percent, 200);
    if (m_danmakuFontSize == percent)
        return;

    m_danmakuFontSize = percent;
    m_settings->setValue(QLatin1String(kKeyDanmakuFontSize), m_danmakuFontSize);
    emit danmakuFontSizeChanged();
}

void AppSettings::setDanmakuSpeed(int speed)
{
    speed = qBound(0, speed, 4);
    if (m_danmakuSpeed == speed)
        return;

    m_danmakuSpeed = speed;
    m_settings->setValue(QLatin1String(kKeyDanmakuSpeed), m_danmakuSpeed);
    emit danmakuSpeedChanged();
}

void AppSettings::setDanmakuScaleWithScreen(bool enabled)
{
    if (m_danmakuScaleWithScreen == enabled)
        return;

    m_danmakuScaleWithScreen = enabled;
    m_settings->setValue(QLatin1String(kKeyDanmakuScaleWithScreen),
                         m_danmakuScaleWithScreen);
    emit danmakuScaleWithScreenChanged();
}

void AppSettings::setMicDevice(const QString &deviceName)
{
    // 空串是合法值（= 跟随系统默认），不作归一化
    if (m_micDevice == deviceName)
        return;

    m_micDevice = deviceName;
    m_settings->setValue(QLatin1String(kKeyMicDevice), m_micDevice);
    emit micDeviceChanged();
}

void AppSettings::setSpeakerDevice(const QString &deviceName)
{
    if (m_speakerDevice == deviceName)
        return;

    m_speakerDevice = deviceName;
    m_settings->setValue(QLatin1String(kKeySpeakerDevice), m_speakerDevice);
    emit speakerDeviceChanged();
}

void AppSettings::setLogLevel(const QString &level)
{
    const QString normalized = level.trimmed().toLower();
    if (m_logLevel == normalized)
        return;

    m_logLevel = normalized;
    m_settings->setValue(QLatin1String(kKeyLogLevel), m_logLevel);
    emit logLevelChanged();

    applyLogLevel();
}

void AppSettings::applyLogLevel()
{
    if (qEnvironmentVariableIsSet("SYNCINE_LOG_LEVEL")) {
        // 环境变量用于临时调整日志，优先级高于 ini，否则开发时设置环境变量
        // 将毫无效果
        LOG_INFO("Settings") << "SYNCINE_LOG_LEVEL 已设置，忽略设置中的日志级别:"
                             << m_logLevel;
        return;
    }

    // 先记录日志再切换级别：级别切换本身需要可见。若顺序颠倒，
    // 调至 warn/error 时本行会被自身过滤掉，日志中无任何痕迹，
    // 无法确认设置是否生效。
    LOG_INFO("Settings") << "日志级别:" << m_logLevel;
    Log::setLevel(levelFromName(m_logLevel));
}
