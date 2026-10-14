#include "AppSettings.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

#include "core/Log.h"

namespace {

// ⚠️ 部署后改这一行:用户没在设置里填地址时就连它。
// 将来要用 wss:// 也在这里改(客户端已经允许 ws/wss 两种 scheme)。
const char *kFallbackServerUrl = "ws://localhost:8765";

const char *kKeyServerUrl = "server/userUrl";
const char *kKeyQuality = "playback/quality";
const char *kKeyHdrAllowed = "playback/hdrAllowed";
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
    // 老版本这里存的是 int(高度上限,0 = 最高);现在存字符串,读法兼容两者:
    // "0" 归一成空串(两种写法语义相同:不设限)
    m_defaultQuality = m_settings->value(QLatin1String(kKeyQuality)).toString().trimmed();
    if (m_defaultQuality == QLatin1String("0"))
        m_defaultQuality.clear();
    m_hdrAllowed = m_settings->value(QLatin1String(kKeyHdrAllowed), false).toBool();
    m_micDevice = m_settings->value(QLatin1String(kKeyMicDevice)).toString();
    m_speakerDevice = m_settings->value(QLatin1String(kKeySpeakerDevice)).toString();
    m_logLevel = m_settings->value(QLatin1String(kKeyLogLevel),
                                   QStringLiteral("info")).toString().toLower();

    LOG_INFO("Settings") << "配置文件:" << filePath();
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
    // Log 那边的文件名是按天的(syncine-2026-10-12.log),这里要的是它所在的目录
    const QString file = Log::filePath();
    return file.isEmpty() ? QString() : QFileInfo(file).absolutePath();
}

void AppSettings::openLogDirectory()
{
    const QString dir = logDirectory();
    if (dir.isEmpty())
        return;

    LOG_INFO("Settings") << "打开日志目录:" << dir;
    // 交给桌面环境用默认的文件管理器打开(打不开也不影响程序,不用管返回值)
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

    // 存 ISO 字符串而不是 QDateTime:ini 里就是纯文本,跨版本也不会变格式
    m_cookieImportedAt = isoTime;
    m_settings->setValue(QLatin1String(kKeyCookieImportedAt), m_cookieImportedAt);
    emit cookieImportedAtChanged();
}

void AppSettings::setDefaultQuality(const QString &quality)
{
    // "0" 和空串是同一个意思(不设限),归一成空串,免得界面两种取值来回跳
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

void AppSettings::setMicDevice(const QString &deviceName)
{
    // 空串是合法值(= 跟随系统默认),不做归一化
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
        // 环境变量是"临时改一下看日志"用的,优先级高于 ini —— 否则开发时
        // 改了环境变量却毫无效果,会怀疑人生
        LOG_INFO("Settings") << "SYNCINE_LOG_LEVEL 已设置,忽略设置里的日志级别:"
                             << m_logLevel;
        return;
    }

    // 先打日志再改级别:"我改了级别"这件事本身要能被看见 ——
    // 反过来的话调成 warn/error 时这行会把自己过滤掉,日志里一片安静,
    // 会让人怀疑设置到底生效没有
    LOG_INFO("Settings") << "日志级别:" << m_logLevel;
    Log::setLevel(levelFromName(m_logLevel));
}
