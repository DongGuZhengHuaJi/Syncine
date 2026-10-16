#include "Toolchain.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include "core/Log.h"

namespace {

QStringList candidateRoots()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList roots;
    roots << appDir + QStringLiteral("/tools");
    roots << QDir::cleanPath(appDir + QStringLiteral("/../tools"));
    roots << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                 + QStringLiteral("/bin");
    return roots;
}

QString shortVersion(const QString &line)
{
    if (line.isEmpty())
        return QStringLiteral("?");
    const QStringList parts = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() >= 3 && parts.at(0) == QLatin1String("ffmpeg")
        && parts.at(1) == QLatin1String("version")) {
        return parts.at(2);
    }
    return line;
}

} // namespace

Toolchain::Toolchain(QObject *parent)
    : QObject(parent) {
}

Toolchain::~Toolchain() = default;

QString Toolchain::findTool(const QString &subdir, const QStringList &names)
{
    for (const QString &root : candidateRoots()) {
        for (const QString &name : names) {
            const QStringList candidates{
                root + QLatin1Char('/') + subdir + QLatin1Char('/') + name,
                root + QLatin1Char('/') + name,
            };

            for (const QString &candidate : candidates) {
                const QFileInfo info(candidate);
                if (!info.exists() || !info.isFile())
                    continue;

                // 从 zip/git 拿到的文件常常没有可执行位，补上再判断
                if (!info.isExecutable()) {
                    QFile::setPermissions(candidate,
                                          QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                              | QFile::ReadGroup | QFile::ExeGroup
                                              | QFile::ReadOther | QFile::ExeOther);
                }
                if (QFileInfo(candidate).isExecutable())
                    return candidate;
            }
        }
    }

    // 最后才退回 PATH。能找到但版本旧的情况要靠调用方把错误原样显示出来 ——
    // 这里只负责"找到",不判断版本够不够新（那得联网问，不该在启动时做）。
    for (const QString &name : names) {
        const QString found = QStandardPaths::findExecutable(name);
        if (!found.isEmpty())
            return found;
    }
    return {};
}

void Toolchain::detect()
{
    // 候选名是**按原样**去 QFileInfo::exists() 查的，不补扩展名，所以带包分发的
    // tools/ 目录在 Windows 上要显式写 .exe（只有末尾 findExecutable() 那条 PATH
    // 回退会按 PATHEXT 补）。非 Windows 上后缀是空串，行为与之前完全一致。
#if defined(Q_OS_WIN)
    const QString exeSuffix = QStringLiteral(".exe");
#else
    const QString exeSuffix = QString();
#endif

    m_ytDlpPath = findTool(QStringLiteral("yt-dlp"),
                           {QStringLiteral("yt-dlp_linux"), QStringLiteral("yt-dlp") + exeSuffix,
                            QStringLiteral("yt-dlp_macos")});
    m_ffmpegPath = findTool(QStringLiteral("ffmpeg"), {QStringLiteral("ffmpeg") + exeSuffix});
    m_ffmpegDir = m_ffmpegPath.isEmpty()
                      ? QString()
                      : QFileInfo(m_ffmpegPath).absolutePath();

    if (m_ytDlpPath.isEmpty())
        LOG_WARN("Toolchain") << "未发现 yt-dlp，资源无法下载";
    else
        LOG_INFO("Toolchain") << "yt-dlp:" << m_ytDlpPath;

    if (m_ffmpegPath.isEmpty())
        LOG_WARN("Toolchain") << "未下发现 ffmpeg，画面/音频分离的资源无法合并";
    else
        LOG_INFO("Toolchain") << "ffmpeg:" << m_ffmpegPath;

    // ffprobe:不是必需的（合并音视频只用 ffmpeg）,但 yt-dlp 修 m3u8 时间戳之类的
    // 场合会用到它 —— 少了它只是降级 + 一条警告。找一下、记进日志，便于排查。
    // 存起来：除了 yt-dlp 修时间戳，客户端的 HDR 检查也用它
    m_ffprobePath = findTool(QStringLiteral("ffmpeg"), {QStringLiteral("ffprobe") + exeSuffix});
    if (!m_ffprobePath.isEmpty())
        LOG_INFO("Toolchain") << "ffprobe:" << m_ffprobePath;
    else if (!m_ffmpegPath.isEmpty())
        LOG_WARN("Toolchain") << "未发现 ffprobe，部分功能受到限制";

    emit statusChanged();

    m_ytDlpVersion.clear();
    m_ffmpegVersion.clear();
    m_pendingProbes = 0;

    // 版本参数每个工具不一样，别想当然：
    //   yt-dlp 用 --version
    //   ffmpeg 只认**单横线**的 -version;写成 --version 会被当成未知选项，
    //   退出码 8
    if (!m_ytDlpPath.isEmpty()) {
        ++m_pendingProbes;
        probeVersion(m_ytDlpPath, {QStringLiteral("--version")}, &m_ytDlpVersion);
    }
    if (!m_ffmpegPath.isEmpty()) {
        ++m_pendingProbes;
        probeVersion(m_ffmpegPath, {QStringLiteral("-version")}, &m_ffmpegVersion);
    }
}

void Toolchain::probeVersion(const QString &path, const QStringList &args, QString *target)
{
    auto *process = new QProcess(this);

    connect(process, &QProcess::finished, this,
            [this, process, target](int exitCode, QProcess::ExitStatus status) {
                if (status == QProcess::NormalExit && exitCode == 0) {
                    QByteArray output = process->readAllStandardOutput();
                    if (output.isEmpty())
                        output = process->readAllStandardError();
                    *target = QString::fromUtf8(output)
                                  .section(QLatin1Char('\n'), 0, 0)
                                  .simplified();
                    LOG_INFO("Toolchain") << QFileInfo(process->program()).fileName()
                                          << "版本:" << shortVersion(*target);
                } else {
                    LOG_WARN("Toolchain") << "执行失败:" << process->program()
                                          << QString::fromUtf8(process->readAllStandardError())
                                                 .section(QLatin1Char('\n'), 0, 0);
                }

                process->deleteLater();
                if (--m_pendingProbes <= 0)
                    emit statusChanged();
            });

    process->start(path, args);
}

bool Toolchain::ytDlpReady() const
{
    return !m_ytDlpPath.isEmpty();
}

bool Toolchain::ffmpegReady() const
{
    return !m_ffmpegPath.isEmpty();
}

bool Toolchain::ready() const
{
    return ytDlpReady() && ffmpegReady();
}

QString Toolchain::statusText() const
{
    if (ready()) {
        return QStringLiteral("工具链就绪(yt-dlp %1 / ffmpeg %2)")
            .arg(shortVersion(m_ytDlpVersion), shortVersion(m_ffmpegVersion));
    }

    QStringList missing;
    if (!ytDlpReady())
        missing << QStringLiteral("yt-dlp");
    if (!ffmpegReady())
        missing << QStringLiteral("ffmpeg");
    return QStringLiteral("缺少 ") + missing.join(QStringLiteral(" 与 "));
}

QStringList Toolchain::ffmpegLocationArgs() const
{
    // 向 yt-dlp 传递 ffmpeg 的位置
    if (m_ffmpegDir.isEmpty())
        return {};
    return {QStringLiteral("--ffmpeg-location"), m_ffmpegDir};
}

bool Toolchain::isSafeMediaUrl(const QString &url)
{
    // 前导空格要去掉再判：argv 里 " -x" 不会被视为选项，但也没必要放过去
    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty())
        return false;

    // 以 '-' 开头的一律拒绝。这是注入的唯一入口，纵使 appendUrlArg 已经插了
    // `--`，这里再挡一道：报错比让 yt-dlp 去解析一个畸形地址清楚得多
    if (trimmed.startsWith(QLatin1Char('-')))
        return false;

    // scheme 白名单。不限定的话，房间成员可以让大家去打开任意本地文件
    // （file://）或触发其他 scheme 的处理程序。
    return trimmed.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)
        || trimmed.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive);
}

void Toolchain::appendUrlArg(QStringList &args, const QString &url)
{
    // `--` 让 yt-dlp（optparse）停止解析选项，其后的元素一律当位置参数，
    // 正常 URL 不受影响。**不要**改成 args << url。
    args << QStringLiteral("--") << url.trimmed();
}
