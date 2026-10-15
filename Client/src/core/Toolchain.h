//
// 外部工具链：yt-dlp 与 ffmpeg 的定位与自检。
//
// 单独成层的原因：这两个是可执行文件而非库，位置随打包方式变化（开发时位于源码树
// 的 tools/ 下，打包后位于可执行文件旁边）；且二者之间存在依赖——yt-dlp 合并音视频
// 时需调用 ffmpeg。将「在哪、是否可用、如何把位置告知 yt-dlp」集中于此，
// 上层下载逻辑无需关心。
//
// 注意与 Qt 自带的 FFmpeg 区分：Qt Multimedia 使用的是库（libavcodec.so 等，
// 随 Qt 分发），从不涉及可执行文件；此处管理的是命令行程序，仅 yt-dlp 会调用。
// 两者互不感知、互不干扰。
//

#ifndef SYNCINE_TOOLCHAIN_H
#define SYNCINE_TOOLCHAIN_H

#include <QObject>
#include <QString>
#include <QStringList>

class QProcess;

class Toolchain : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool ytDlpReady
               READ ytDlpReady
               NOTIFY statusChanged)

    Q_PROPERTY(bool ffmpegReady
               READ ffmpegReady
               NOTIFY statusChanged)

    // 所有工具都就绪
    Q_PROPERTY(bool ready
               READ ready
               NOTIFY statusChanged)

    // 工具链状态
    Q_PROPERTY(QString statusText
               READ statusText
               NOTIFY statusChanged)

public:
    explicit Toolchain(QObject *parent = nullptr);
    ~Toolchain() override;

    // 启动后调用一次：查找文件 + 校验可执行 + 读取版本号。
    // 全程异步（yt-dlp 为 PyInstaller 打包，启动需数百毫秒，不能阻塞界面）。
    void detect();

    bool ytDlpReady() const;
    bool ffmpegReady() const;
    bool ready() const;

    QString statusText() const;

    QString ytDlpPath() const { return m_ytDlpPath; }
    QString ffmpegPath() const { return m_ffmpegPath; }
    // ffprobe 与 ffmpeg 一并查找，可能为空（缺失仅降级，不致命）
    QString ffprobePath() const { return m_ffprobePath; }
    QString ytDlpVersion() const { return m_ytDlpVersion; }
    QString ffmpegVersion() const { return m_ffmpegVersion; }

    // 传给 yt-dlp 的参数：显式指定使用本项目的这一份 ffmpeg。
    //
    // 必须显式指定的原因：yt-dlp 的查找顺序为
    //   本参数 → 与自身同目录 → 系统 PATH
    // 用户机器上只要存在旧版 ffmpeg（如发行版自带的 4.x），就会被优先采用——
    // 旧版本转封装兼容性较差，且报错信息难以理解。
    QStringList ffmpegLocationArgs() const;

signals:
    void statusChanged();

private:
    // 按候选目录与候选文件名查找可执行文件；未找到返回空串。
    // 找到但缺少可执行位时会补上（从 zip/git 获取的文件会丢失该位）。
    static QString findTool(const QString &subdir, const QStringList &names);
    // 异步读取版本号（输出的第一行）。各工具的 args 不同，见 detect() 中的注释。
    void probeVersion(const QString &path, const QStringList &args, QString *target);

    QString m_ytDlpPath;
    QString m_ffmpegPath;
    QString m_ffprobePath;
    QString m_ffmpegDir;
    QString m_ytDlpVersion;
    QString m_ffmpegVersion;
    int m_pendingProbes = 0;
};

#endif //SYNCINE_TOOLCHAIN_H
