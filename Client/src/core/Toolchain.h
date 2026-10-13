//
// 外部工具链:yt-dlp 与 ffmpeg 的定位与自检。
//
// 为什么单独一层:
//   这两个是**可执行文件**而不是库,位置随打包方式变(开发时在源码树的 tools/ 下、
//   打包后在可执行文件旁边);而且它们之间还有依赖——yt-dlp 合并音视频时要调用 ffmpeg。
//   把"在哪、能不能用、怎么把位置告诉 yt-dlp"集中在这一处,上层的下载逻辑就不必关心。
//
// 注意和 Qt 自带的那份 FFmpeg 区分开:
//   Qt Multimedia 用的是**库**(libavcodec.so 等,随 Qt 分发),从不碰可执行文件;
//   这里管的是**命令行程序**,只有 yt-dlp 会调用它。两者互不感知、互不干扰。
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

    // 启动后调一次:找文件 + 校验可执行 + 读版本号。
    // 全程异步(yt-dlp 是 PyInstaller 打的包,启动要几百毫秒,不能卡住界面)。
    void detect();

    bool ytDlpReady() const;
    bool ffmpegReady() const;
    bool ready() const;

    QString statusText() const;

    QString ytDlpPath() const { return m_ytDlpPath; }
    QString ffmpegPath() const { return m_ffmpegPath; }
    // ffprobe 和 ffmpeg 一起找,可能为空(缺了只是降级,不是致命)
    QString ffprobePath() const { return m_ffprobePath; }
    QString ytDlpVersion() const { return m_ytDlpVersion; }
    QString ffmpegVersion() const { return m_ffmpegVersion; }

    // 传给 yt-dlp 的参数:显式告诉它用我们这一份 ffmpeg。
    //
    // 为什么必须显式指定,而不是指望它自己找:yt-dlp 的查找顺序是
    //   本参数 → 跟它自己同目录 → 系统 PATH
    // 用户机器上只要有个旧 ffmpeg(比如发行版自带的 4.x),就会优先用那个 ——
    // 老版本转封装的兼容性差,报出来的错还很难懂。
    QStringList ffmpegLocationArgs() const;

signals:
    void statusChanged();

private:
    // 按候选目录和候选文件名找一个可执行文件;找不到返回空串。
    // 找到但缺可执行位时会补上(从 zip/git 拿到的文件会丢这个位)。
    static QString findTool(const QString &subdir, const QStringList &names);
    // 异步取版本号(输出的第一行)。args 每个工具不同,见 detect() 里的注释。
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
