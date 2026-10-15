#include "HdrConverter.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

#include "core/Log.h"
#include "core/Toolchain.h"

namespace {

// 色调映射链。这是 mpv 那套（libplacebo）,不是手搓的 —— 实测差距很大：
//   libplacebo bt.2390:亮度 167.36 / 彩度 14.43(无损参考是 167.34 / 14.45)
//   手搓 hable+npl=100:亮度 150.04 / 彩度 13.86(亮度低 10%)
// 三个关键点：
//   gamut_mode=perceptual —— BT.2020 → BT.709 按感知映射，而不是硬裁
//   peak_detect=1  —— 逐帧检测实际峰值亮度（动态映射）
//   format=yuv420p —— 输出 8bit SDR
const char *kTonemapFilter =
    "libplacebo=tonemapping=bt.2390:gamut_mode=perceptual:peak_detect=1:"
    "colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv:format=yuv420p";

// 目标码率的上下限（kbps）。下限 4000:再低 1080p 的动态画面会糊；
// 上限 16000:再高对"看一遍"没意义，只会把文件撑大
constexpr int kMinBitrateKbps = 4000;
constexpr int kMaxBitrateKbps = 16000;

constexpr int kProbeTimeoutMs = 8000;

} // namespace

HdrConverter::HdrConverter(const Toolchain *toolchain, QObject *parent)
    : QObject(parent),
      m_toolchain(toolchain) {
}

HdrConverter::~HdrConverter()
{
    for (QProcess *process : { m_probe, m_convert }) {
        if (process != nullptr) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
    // 析构里不打日志：main() 里这些对象是在 Log::shutdown() 之后才析构的
}

bool HdrConverter::promptVisible() const
{
    return m_hdr && !m_dismissed && m_convert == nullptr && !m_sourcePath.isEmpty();
}

qint64 HdrConverter::estimatedBytes() const
{
    if (m_durationMs <= 0)
        return 0;
    // 码率（kbps） × 时长（秒） / 8 → 字节
    return static_cast<qint64>(targetBitrateKbps()) * 1000 / 8 * (m_durationMs / 1000);
}

QUrl HdrConverter::outputUrl() const
{
    return m_outputPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(m_outputPath);
}

int HdrConverter::targetBitrateKbps() const
{
    // 跟着源走（×2 留余量）,但夹在上下限之间。
    // 实测：2.5Mbps 就已经和"未经编码的滤波输出"测不出差别，4M 是留了安全边际
    const int fromSource = static_cast<int>(m_sourceBitrateBps * 2 / 1000);
    return qBound(kMinBitrateKbps, fromSource, kMaxBitrateKbps);
}

// ============================
// 检查
// ============================

void HdrConverter::check(const QUrl &fileUrl)
{
    if (m_convert != nullptr)
        return;   // 正在转,别打断

    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty()) {
        // 远端画面 / 直链流：没有本地文件可查，清空状态
        if (!m_sourcePath.isEmpty()) {
            resetForNewSource();
            emit stateChanged();
        }
        return;
    }
    if (path == m_sourcePath)
        return;   // 同一个文件,别重复探测

    resetForNewSource();
    m_sourcePath = path;
    emit stateChanged();

    if (m_toolchain == nullptr || !m_toolchain->ffmpegReady()) {
        LOG_WARN("Hdr") << "没有 ffmpeg,跳过 HDR 检查";
        return;
    }

    m_probe = new QProcess(this);
    connect(m_probe, &QProcess::finished, this,
            [this](int, QProcess::ExitStatus) { handleProbeFinished(); });
    connect(m_probe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            LOG_WARN("Hdr") << "ffprobe 起不来";
            m_probe->deleteLater();
            m_probe = nullptr;
        }
    });

    // 只问需要的三个字段：传输曲线（判断 HDR）、时长、码率（定转码目标）
    m_probe->setProgram(m_toolchain->ffprobePath());
    m_probe->setArguments({
        QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-select_streams"), QStringLiteral("v:0"),
        QStringLiteral("-show_entries"), QStringLiteral("stream=color_transfer"),
        QStringLiteral("-show_entries"), QStringLiteral("format=duration,bit_rate"),
        QStringLiteral("-of"), QStringLiteral("default=noprint_wrappers=1"),
        path,
    });

    LOG_DEBUG("Hdr") << "检查是否 HDR:" << path;
    m_probe->start();

    // 卡住的话得有个出口 —— 检查失败就当作"不是 HDR",不挡播放
    QTimer::singleShot(kProbeTimeoutMs, this, [this]() {
        if (m_probe != nullptr && m_probe->state() != QProcess::NotRunning) {
            LOG_WARN("Hdr") << "ffprobe 超时";
            m_probe->kill();
        }
    });
}

void HdrConverter::handleProbeFinished()
{
    if (m_probe == nullptr)
        return;

    const QString output = QString::fromUtf8(m_probe->readAllStandardOutput());
    m_probe->deleteLater();
    m_probe = nullptr;

    bool isHdr = false;
    for (const QString &line : output.split(QLatin1Char('\n'))) {
        const QString text = line.trimmed();
        if (text.startsWith(QLatin1String("color_transfer="))) {
            const QString transfer = text.section(QLatin1Char('='), 1).trimmed();
            // PQ(HDR10)和 HLG 是两种最常见的 HDR 曲线
            isHdr = (transfer == QLatin1String("smpte2084")
                     || transfer == QLatin1String("arib-std-b67"));
        } else if (text.startsWith(QLatin1String("duration="))) {
            m_durationMs = static_cast<qint64>(
                text.section(QLatin1Char('='), 1).trimmed().toDouble() * 1000);
        } else if (text.startsWith(QLatin1String("bit_rate="))) {
            // stream 的 bit_rate 经常是 N/A,那行会被 ffprobe 直接省略；
            // format 的那条一般有，当兜底
            const qint64 rate = text.section(QLatin1Char('='), 1).trimmed().toLongLong();
            if (rate > 0)
                m_sourceBitrateBps = rate;
        }
    }

    m_hdr = isHdr;
    if (m_hdr) {
        LOG_INFO("Hdr") << "这是 HDR 视频(播放会过曝):" << QFileInfo(m_sourcePath).fileName()
                        << "时长" << m_durationMs / 1000 << "秒,源码率"
                        << m_sourceBitrateBps / 1000 << "kbps";
    } else {
        LOG_DEBUG("Hdr") << "不是 HDR:" << QFileInfo(m_sourcePath).fileName();
    }
    emit stateChanged();
}

// ============================
// 转换
// ============================

void HdrConverter::convert()
{
    if (m_convert != nullptr || !m_hdr || m_sourcePath.isEmpty())
        return;
    if (m_toolchain == nullptr || !m_toolchain->ffmpegReady()) {
        fail(QStringLiteral("找不到 ffmpeg,没法转码"));
        return;
    }

    // 输出放在源文件旁边，名字加 _SDR 后缀
    const QFileInfo info(m_sourcePath);
    m_outputPath = info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
                   + QStringLiteral("_SDR.mkv");
    m_outputTail.clear();
    m_progressBuffer.clear();
    m_progress = 0;
    m_errorText.clear();

    m_convert = new QProcess(this);
    // **不要合并通道**:进度走 stdout(-progress pipe:1),错误走 stderr,分开读
    m_convert->setProgram(m_toolchain->ffmpegPath());
    m_convert->setArguments({
        QStringLiteral("-hide_banner"),
        QStringLiteral("-nostdin"),   // 否则 ffmpeg 会抢标准输入
        QStringLiteral("-y"),
        QStringLiteral("-i"), m_sourcePath,
        QStringLiteral("-vf"), QString::fromLatin1(kTonemapFilter),
        // LGPL 构建里没有 x264(GPL),用 libopenh264(BSD)
        QStringLiteral("-c:v"), QStringLiteral("libopenh264"),
        QStringLiteral("-b:v"), QStringLiteral("%1k").arg(targetBitrateKbps()),
        QStringLiteral("-c:a"), QStringLiteral("aac"),
        QStringLiteral("-b:a"), QStringLiteral("192k"),
        QStringLiteral("-progress"), QStringLiteral("pipe:1"),
        QStringLiteral("-nostats"),
        m_outputPath,
    });

    connect(m_convert, &QProcess::readyReadStandardOutput, this, [this]() {
        m_progressBuffer += QString::fromUtf8(m_convert->readAllStandardOutput());
        // 按行切开处理，最后一段不完整的留在缓冲里等下一批
        int newline = -1;
        while ((newline = m_progressBuffer.indexOf(QLatin1Char('\n'))) >= 0) {
            handleProgressLine(m_progressBuffer.left(newline).trimmed());
            m_progressBuffer.remove(0, newline + 1);
        }
    });
    connect(m_convert, &QProcess::readyReadStandardError, this, [this]() {
        m_outputTail += QString::fromUtf8(m_convert->readAllStandardError());
        // 只留尾巴 —— 失败时给用户看的是最后几行
        const QStringList lines = m_outputTail.split(QLatin1Char('\n'));
        if (lines.size() > 20)
            m_outputTail = lines.mid(lines.size() - 20).join(QLatin1Char('\n'));
    });
    connect(m_convert, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(QStringLiteral("ffmpeg 启动失败:%1").arg(m_convert->errorString()));
    });
    connect(m_convert, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        handleConvertFinished(exitCode);
    });

    LOG_INFO("Hdr") << "开始转 SDR:" << QFileInfo(m_sourcePath).fileName()
                    << "→" << QFileInfo(m_outputPath).fileName()
                    << "目标码率" << targetBitrateKbps() << "kbps";
    m_convert->start();
    emit stateChanged();
}

void HdrConverter::handleProgressLine(const QString &line)
{
    // -progress 的输出形如：
    //     out_time_us=5088417
    //     speed=3.39x
    //     progress=continue
    if (!line.startsWith(QLatin1String("out_time_us=")))
        return;
    if (m_durationMs <= 0)
        return;

    const qint64 micros = line.section(QLatin1Char('='), 1).trimmed().toLongLong();
    const int percent = std::clamp(
        static_cast<int>(micros * 100 / (m_durationMs * 1000)), 0, 100);
    if (percent == m_progress)
        return;   // 别为了每 1% 以内的变化刷界面

    m_progress = percent;
    emit stateChanged();
}

void HdrConverter::handleConvertFinished(int exitCode)
{
    // 别把局部变量叫 finished —— 会和同名信号撞上，`emit finished(...)` 直接编译不过
    QProcess *process = m_convert;
    m_convert = nullptr;
    if (process != nullptr)
        process->deleteLater();

    const QFileInfo output(m_outputPath);
    // 不看退出码：ffmpeg 有时非零退出但产物是完整的 —— 以"文件在且不为空"为准
    if (exitCode != 0 || !output.exists() || output.size() <= 0) {
        const QStringList lines = m_outputTail.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        fail(lines.isEmpty() ? QStringLiteral("转码失败(ffmpeg 退出码 %1)").arg(exitCode)
                             : lines.mid(qMax(0, lines.size() - 3)).join(QLatin1Char('\n')));
        return;
    }

    m_progress = 100;
    LOG_INFO("Hdr") << "转码完成:" << output.fileName()
                    << QStringLiteral("%1 MB").arg(output.size() / 1024.0 / 1024.0, 0, 'f', 1);
    emit stateChanged();
    emit finished(outputUrl());
}

void HdrConverter::cancel()
{
    if (m_convert == nullptr)
        return;

    LOG_INFO("Hdr") << "取消转码";
    m_convert->kill();
    // 半截的产物删掉，免得下次被当成"转好的"
    QFile::remove(m_outputPath);
    m_outputPath.clear();
    m_progress = -1;
    emit stateChanged();
}

void HdrConverter::dismiss()
{
    if (!m_hdr || m_dismissed)
        return;

    LOG_DEBUG("Hdr") << "用户选择继续看原片";
    m_dismissed = true;
    emit stateChanged();
}

// ============================
// 内部
// ============================

void HdrConverter::resetForNewSource()
{
    m_sourcePath.clear();
    m_outputPath.clear();
    m_hdr = false;
    m_dismissed = false;
    m_progress = -1;
    m_durationMs = 0;
    m_sourceBitrateBps = 0;
    m_errorText.clear();
    m_outputTail.clear();
    m_progressBuffer.clear();
}

void HdrConverter::fail(const QString &message)
{
    m_errorText = message;
    m_progress = -1;
    LOG_WARN("Hdr") << "转码失败:" << message;
    emit stateChanged();
    emit failed(message);
}
