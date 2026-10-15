#include "CookieManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

#include "core/AppSettings.h"
#include "core/Log.h"
#include "core/Toolchain.h"

namespace {

// 导出时给 yt-dlp 的"URL"。它必须有个 URL 才肯干活,而这个不合法 scheme
// 会让它立刻报错退出(**不触网**,1 秒内结束)—— 但 cookie jar 照样在退出时
// 落盘,这正是我们要的。
// **副作用是退出码永远是 1** —— 所以判成功绝不能看退出码,只能看文件。
const char *kDummyUrl = "bogus-scheme://nothing";

// 超过这么多天就提醒用户"该重新导一次了"。cookie 会过期,快照更是。
constexpr int kStaleDays = 30;

// 导出/导入的看门狗:keyring 弹窗、浏览器数据库被锁死时得有个出口
constexpr int kWatchdogMs = 90 * 1000;

const char *kNetscapeMagic = "# Netscape HTTP Cookie File";

struct BrowserEntry {
    const char *key;    // 喂给 yt-dlp --cookies-from-browser 的值
    const char *label;  // 界面上显示的名字
    QStringList dirs;   // profile 目录候选(存在任意一个就算"装了")
};

// 启发式搜索本地浏览器目录
QList<BrowserEntry> browserTable()
{
    const QString home = QDir::homePath();

#if defined(Q_OS_LINUX)

    // Linux：普通安装、Snap、Flatpak
    return {
        {
            "firefox", "Firefox",
            {
                home + QStringLiteral("/.mozilla/firefox"),
                home + QStringLiteral("/snap/firefox/common/.mozilla/firefox"),
                home + QStringLiteral("/.var/app/org.mozilla.firefox/.mozilla/firefox")
            }
        },
        {
            "chrome", "Chrome",
            {
                home + QStringLiteral("/.config/google-chrome")
            }
        },
        {
            "chromium", "Chromium",
            {
                home + QStringLiteral("/.config/chromium"),
                home + QStringLiteral("/snap/chromium/common/chromium"),
                home + QStringLiteral("/.var/app/org.chromium.Chromium/config/chromium")
            }
        },
        {
            "edge", "Edge",
            {
                home + QStringLiteral("/.config/microsoft-edge")
            }
        },
        {
            "brave", "Brave",
            {
                home + QStringLiteral("/.config/BraveSoftware/Brave-Browser")
            }
        },
        {
            "vivaldi", "Vivaldi",
            {
                home + QStringLiteral("/.config/vivaldi")
            }
        },
        {
            "opera", "Opera",
            {
                home + QStringLiteral("/.config/opera")
            }
        }
    };

#elif defined(Q_OS_WIN)

    // Windows：浏览器通常将用户数据保存在 AppData/Local 下。
    const QString localAppData =
        qEnvironmentVariable("LOCALAPPDATA");

    return {
        {
            "firefox", "Firefox",
            {
                home + QStringLiteral("/AppData/Roaming/Mozilla/Firefox")
            }
        },
        {
            "chrome", "Chrome",
            {
                localAppData + QStringLiteral("/Google/Chrome/User Data")
            }
        },
        {
            "chromium", "Chromium",
            {
                localAppData + QStringLiteral("/Chromium/User Data")
            }
        },
        {
            "edge", "Edge",
            {
                localAppData + QStringLiteral("/Microsoft/Edge/User Data")
            }
        },
        {
            "brave", "Brave",
            {
                localAppData + QStringLiteral("/BraveSoftware/Brave-Browser/User Data")
            }
        },
        {
            "vivaldi", "Vivaldi",
            {
                localAppData + QStringLiteral("/Vivaldi/User Data")
            }
        },
        {
            "opera", "Opera",
            {
                qEnvironmentVariable("APPDATA")
                    + QStringLiteral("/Opera Software/Opera Stable")
            }
        }
    };

#elif defined(Q_OS_MACOS)

    // macOS：浏览器通常将用户数据保存在 ~/Library/Application Support 下。
    const QString appSupport =
        home + QStringLiteral("/Library/Application Support");

    return {
        {
            "firefox", "Firefox",
            {
                home + QStringLiteral("/Library/Application Support/Firefox")
            }
        },
        {
            "chrome", "Chrome",
            {
                appSupport + QStringLiteral("/Google/Chrome")
            }
        },
        {
            "chromium", "Chromium",
            {
                appSupport + QStringLiteral("/Chromium")
            }
        },
        {
            "edge", "Edge",
            {
                appSupport + QStringLiteral("/Microsoft Edge")
            }
        },
        {
            "brave", "Brave",
            {
                appSupport + QStringLiteral("/BraveSoftware/Brave-Browser")
            }
        },
        {
            "vivaldi", "Vivaldi",
            {
                appSupport + QStringLiteral("/Vivaldi")
            }
        },
        {
            "opera", "Opera",
            {
                appSupport + QStringLiteral("/com.operasoftware.Opera")
            }
        }
    };

#else

    // 未支持的平台
    return {};

#endif
}

QString labelForKey(const QString &key)
{
    for (const BrowserEntry &entry : browserTable()) {
        if (key == QLatin1String(entry.key))
            return QString::fromUtf8(entry.label);
    }
    return key;
}

// 数 cookie 条目,并顺手把首行(魔数)带出来。
//
// 规则:7 列 Tab 分隔的行才算一条。
// **`#HttpOnly_` 开头的是有效条目,不是注释**(这是 Netscape 格式的约定),
// 跳注释时别把它一起跳了。
int countEntries(const QString &path, QString *firstLine)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return -1;

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);

    bool first = true;
    int count = 0;
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        if (first) {
            // 带 BOM 的文件会让魔数检查失败,这里先剥掉
            line.remove(QChar(0xFEFF));
            if (firstLine != nullptr)
                *firstLine = line;
            first = false;
        }

        if (line.startsWith(QLatin1Char('#'))
            && !line.startsWith(QLatin1String("#HttpOnly_")))
            continue;
        if (line.count(QLatin1Char('\t')) == 6)  // 7 列 = 6 个分隔符
            ++count;
    }
    return count;
}

// 这些错误值得翻译成人话;翻译表覆盖不到的,退回 yt-dlp 的原话
QString humanizeError(const QString &output, const QString &browserLabel)
{
    const QString lower = output.toLower();

    auto has = [&lower](const char *needle) {
        return lower.contains(QLatin1String(needle));
    };

    if (has("could not copy"))
        return QStringLiteral("读不了 %1 的 cookie 数据库(它可能正在运行)。"
                              "完全退出 %1(含后台驻留进程)后重试").arg(browserLabel);
    if (has("could not find") && has("cookies database"))
        return QStringLiteral("没找到 %1 的 cookie 数据库。"
                              "先打开一次 %1 并登录相关网站,再回来导入").arg(browserLabel);
    if (has("keyring") || has("cannot decrypt") || has("failed to decrypt")
        || has("no key found"))
        return QStringLiteral("无法解密 %1 的 cookie(系统密钥环没解锁或不可用)。"
                              "解锁密钥环后重试,或者改用 Firefox / 手动导入文件")
            .arg(browserLabel);
    if (has("netscape"))
        return QStringLiteral("这不是 Netscape 格式的 cookie 文件 —— "
                              "要用「Get cookies.txt」这类扩展导出的 .txt");
    if (has("permissionerror") || has("filenotfounderror") || has("traceback"))
        return QStringLiteral("写 cookie 文件失败(磁盘或权限问题)。"
                              "换个位置或检查磁盘剩余空间后重试");

    // 兜底:取最后一条 ERROR: 行;没有就取输出的最后几行(防止 traceback 糊屏)
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines.at(i).trimmed();
        if (line.startsWith(QLatin1String("ERROR:")))
            return line;
    }
    return lines.mid(qMax(0, lines.size() - 3)).join(QLatin1Char('\n')).trimmed();
}

} // namespace

CookieManager::CookieManager(const Toolchain *toolchain, AppSettings *settings,
                             QObject *parent)
    : QObject(parent),
      m_toolchain(toolchain),
      m_settings(settings) {

    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, [this]() {
        if (m_process == nullptr)
            return;
        LOG_WARN("Cookie") << "导入超时,掐掉 yt-dlp";
        m_timedOut = true;
        m_process->kill();
        fail(QStringLiteral("导入超时,请重试"));
    });

    // 目录先建出来:yt-dlp 写不存在的目录是**崩在 Python 里**,一个字都不落
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!dir.isEmpty())
        QDir().mkpath(dir);

    // 每次下载 yt-dlp 都会回写这个文件(它退出时把 cookie jar 存回去),
    // 所以状态得跟着文件走 —— 不然界面上会一直显示已经删掉的旧状态
    if (!path().isEmpty()) {
        m_watcher.addPath(path());
        connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this]() {
            // 被下载重新创建出来的文件是 0644(没有我们的预创建),重贴一次 600;
            // 文件可能被删掉,watcher 会自己移除路径,重新观察交给 refreshState
            if (QFile::exists(path())) {
                QFile::setPermissions(path(), QFile::ReadOwner | QFile::WriteOwner);
                if (!m_watcher.files().contains(path()))
                    m_watcher.addPath(path());
            }
            refreshState();
        });
    }

    refreshBrowsers();
    refreshState();
}

CookieManager::~CookieManager()
{
    if (m_process != nullptr) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
    // 半截的临时文件不该留下。
    // **这里不要打日志** —— main() 里这些对象是在 Log::shutdown() 之后才析构的
    QFile::remove(path() + QStringLiteral(".part"));
}

QString CookieManager::path() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/cookies.txt");
}

QString CookieManager::source() const
{
    return m_settings != nullptr ? m_settings->cookieSource() : QString();
}

QString CookieManager::importedAt() const
{
    return m_settings != nullptr ? m_settings->cookieImportedAt() : QString();
}

bool CookieManager::stale() const
{
    if (!m_ready || importedAt().isEmpty())
        return false;

    const QDateTime when = QDateTime::fromString(importedAt(), Qt::ISODate);
    if (!when.isValid())
        return false;
    return when.daysTo(QDateTime::currentDateTime()) >= kStaleDays;
}

// ============================
// 浏览器探测
// ============================

void CookieManager::refreshBrowsers()
{
    QVariantList list;
    for (const BrowserEntry &entry : browserTable()) {
        bool found = false;
        for (const QString &dir : entry.dirs) {
            if (QFileInfo::exists(dir)) {
                found = true;
                break;
            }
        }
        if (!found)
            continue;

        QVariantMap item;
        item[QStringLiteral("label")] = QString::fromUtf8(entry.label);
        item[QStringLiteral("value")] = QString::fromLatin1(entry.key);
        list.append(item);
    }

    if (m_browsers == list)
        return;

    m_browsers = list;
    QStringList labels;
    for (const QVariant &item : list)
        labels << item.toMap().value(QStringLiteral("label")).toString();
    LOG_INFO("Cookie") << "检测到浏览器:" << (labels.isEmpty()
                                                  ? QStringLiteral("(无)")
                                                  : labels.join(QLatin1Char('/')));
    emit browsersChanged();
    emit stateChanged();   // "一个都没检测到"的提示也靠这个刷新
}

// ============================
// 导入:从浏览器
// ============================

void CookieManager::importFromBrowser(const QString &key)
{
    if (m_process != nullptr) {
        LOG_DEBUG("Cookie") << "上一次导入还没结束,忽略这次";
        return;
    }
    if (m_toolchain == nullptr || !m_toolchain->ytDlpReady()) {
        fail(QStringLiteral("找不到 yt-dlp,没法从浏览器读取 cookie(可以改用「从文件导入」)"));
        return;
    }

    // 先建好一个 0600 的空壳:yt-dlp 退出时会**原地重写**它,权限保持不变。
    // 反过来让它自己创建,拿到的是 0644(全组可读)
    QString error;
    if (!writePartial(QByteArray(), &error)) {
        fail(error);
        return;
    }

    m_importingKey = key;
    m_output.clear();
    m_errorText.clear();

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->setProgram(m_toolchain->ytDlpPath());

    const QStringList args{
        QStringLiteral("--ignore-config"),   // 用户 ~/.config/yt-dlp/config 里的
        QStringLiteral("--no-cache-dir"),    // --cookies 会在退出时被我们覆盖掉
        QStringLiteral("--no-warnings"),
        QStringLiteral("--cookies-from-browser"), key,
        QStringLiteral("--cookies"), path() + QStringLiteral(".part"),
        QStringLiteral("--simulate"),
        QStringLiteral("--no-playlist"),
        QString::fromLatin1(kDummyUrl),
    };
    m_process->setArguments(args);

    LOG_INFO("Cookie") << "从" << labelForKey(key) << "导入 cookie →" << path();

    connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        m_output += QString::fromUtf8(m_process->readAllStandardOutput());
    });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    fail(QStringLiteral("yt-dlp 启动失败:%1").arg(m_process->errorString()));
            });
    connect(m_process, &QProcess::finished, this,
            [this](int exitCode, QProcess::ExitStatus) { handleBrowserFinished(exitCode); });

    emit stateChanged();
    m_process->start();
    m_watchdog.start(kWatchdogMs);
}

void CookieManager::handleBrowserFinished(int exitCode)
{
    QProcess *finished = m_process;
    m_process = nullptr;
    m_watchdog.stop();
    if (finished != nullptr)
        finished->deleteLater();

    // 超时时已经报过错了(那次是被我们 kill 的),这里别再覆盖一遍
    if (m_timedOut) {
        m_timedOut = false;
        QFile::remove(path() + QStringLiteral(".part"));
        emit stateChanged();
        return;
    }

    const QString label = labelForKey(m_importingKey);

    // **不能看退出码**:假 URL 必然让 yt-dlp 报错退出(exit=1),而 cookie
    // 照样是导出了的。唯一的判据是文件本身。
    if (exitCode != 0 && m_output.contains(QLatin1String("Extracted 0 cookies"))) {
        fail(QStringLiteral("%1 里没有可用 cookie(可能没登录过任何网站)").arg(label));
        QFile::remove(path() + QStringLiteral(".part"));
        return;
    }

    QString error;
    if (!finalizePartial(&error)) {
        fail(m_output.trimmed().isEmpty() ? error : humanizeError(m_output, label));
        return;
    }

    if (m_settings != nullptr) {
        m_settings->setLastCookieBrowser(m_importingKey);
        m_settings->setCookieSource(label);
        m_settings->setCookieImportedAt(QDateTime::currentDateTime().toString(Qt::ISODate));
    }

    refreshState();
    LOG_INFO("Cookie") << "导入成功:" << m_cookieCount << "条,来自" << label;
    emit stateChanged();
}

// ============================
// 导入:从文件
// ============================

void CookieManager::importFromFile(const QUrl &fileUrl)
{
    if (m_process != nullptr) {
        LOG_DEBUG("Cookie") << "正在导入,忽略这次";
        return;
    }

    // FileDialog 给的是 url,不是路径 —— 直接当字符串用会得到 "file:///..."
    const QString source = fileUrl.toLocalFile();
    m_errorText.clear();
    if (source.isEmpty() || !QFileInfo::exists(source)) {
        fail(QStringLiteral("读不了这个文件"));
        return;
    }
    if (source == path()) {
        refreshState();   // 选的就是我们自己那份,当作成功
        emit stateChanged();
        return;
    }

    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("读不了这个文件(没有读权限?)"));
        return;
    }
    const QByteArray content = file.readAll();
    file.close();

    QString error;
    if (!writePartial(content, &error) || !finalizePartial(&error)) {
        fail(error);
        return;
    }

    if (m_settings != nullptr) {
        m_settings->setCookieSource(QStringLiteral("手动导入"));
        m_settings->setCookieImportedAt(QDateTime::currentDateTime().toString(Qt::ISODate));
    }

    refreshState();
    LOG_INFO("Cookie") << "手动导入成功:" << m_cookieCount << "条";
    emit stateChanged();
}

void CookieManager::clear()
{
    if (busy())
        return;

    if (!QFile::remove(path()))
        LOG_WARN("Cookie") << "删除 cookie 文件失败(可能本来就不存在)";

    // 元信息也要清 —— 否则界面会显示"已导入 1846 条"而文件已经没了
    if (m_settings != nullptr) {
        m_settings->setCookieSource(QString());
        m_settings->setCookieImportedAt(QString());
    }
    m_errorText.clear();
    refreshState();
    LOG_INFO("Cookie") << "已清除 cookie";
    emit stateChanged();
}

// ============================
// 文件读写
// ============================

bool CookieManager::writePartial(const QByteArray &content, QString *error)
{
    const QString partPath = path() + QStringLiteral(".part");
    QFile::remove(partPath);   // 上一轮失败留下的半截文件

    QFile file(partPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("写不了 %1(磁盘或权限问题)").arg(partPath);
        return false;
    }

    // open() 建出来的文件权限受 umask 影响(通常 0644),立刻收紧到 0600。
    // 从文件导入时,这一步是权限的全部保证;从浏览器导入时,yt-dlp 之后是
    // **原地重写**这个文件(不改权限),所以也是安全的
    QFile::setPermissions(partPath, QFile::ReadOwner | QFile::WriteOwner);

    // 空内容 = 只建个合法的壳给 yt-dlp 重写;有内容 = 手动导入,先剥 BOM
    QByteArray payload = content;
    if (payload.startsWith("\xEF\xBB\xBF"))
        payload.remove(0, 3);

    if (payload.isEmpty())
        payload = QByteArray(kNetscapeMagic) + "\n";

    file.write(payload);
    file.close();
    return true;
}

bool CookieManager::finalizePartial(QString *error)
{
    const QString partPath = path() + QStringLiteral(".part");

    QString firstLine;
    const int count = countEntries(partPath, &firstLine);
    if (count < 0) {
        QFile::remove(partPath);
        *error = QStringLiteral("写 cookie 文件失败(读不出来)");
        return false;
    }
    if (!firstLine.startsWith(QLatin1String(kNetscapeMagic))) {
        QFile::remove(partPath);
        *error = QStringLiteral("这不是 Netscape 格式的 cookie 文件 —— "
                                "要用「Get cookies.txt」这类扩展导出的 .txt");
        return false;
    }
    if (count == 0) {
        QFile::remove(partPath);
        *error = QStringLiteral("这个文件里没有 cookie");
        return false;
    }

    // 改名:先删旧的(同目录改名在 POSIX 上本来就是原子的,Qt 的 rename
    // 不覆盖已存在文件,所以得先 remove)。这一步保证正在跑的下载要么看到
    // 旧文件、要么看到新文件,不会看到半截 —— 也保证**导入失败时旧文件完好**
    QFile::remove(path());
    if (!QFile::rename(partPath, path())) {
        QFile::remove(partPath);
        *error = QStringLiteral("替换 cookie 文件失败");
        return false;
    }

    QFile::setPermissions(path(), QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

void CookieManager::refreshState()
{
    m_cookieCount = 0;
    m_ready = false;

    const QString file = path();
    if (!file.isEmpty() && QFileInfo::exists(file)) {
        QString firstLine;
        const int count = countEntries(file, &firstLine);
        if (count > 0 && firstLine.startsWith(QLatin1String(kNetscapeMagic))) {
            m_cookieCount = count;
            m_ready = true;
        }
    }

    // 文件可能刚被创建出来(下载回写),把 watcher 补上
    if (!file.isEmpty() && QFileInfo::exists(file) && !m_watcher.files().contains(file))
        m_watcher.addPath(file);

    emit stateChanged();
}

void CookieManager::fail(const QString &text)
{
    m_errorText = text;
    LOG_WARN("Cookie") << "导入失败:" << text;
    emit stateChanged();
}

QStringList CookieManager::cookieArgs() const
{
    if (!m_ready || path().isEmpty())
        return {};
    return { QStringLiteral("--cookies"), path() };
}
