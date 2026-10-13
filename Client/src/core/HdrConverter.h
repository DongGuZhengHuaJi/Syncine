//
// HDR 视频 → SDR 转换。
//
// 为什么需要:Qt 的渲染**不做 HDR→SDR 色调映射** —— 它把 PQ 曲线解开成线性后
// 直接截断在 SDR 白(实测:76% 的通道值压到顶、63% 的像素纯白)。表现就是
// "暗处正常、亮处过曝"。详见 memory 里的 syncine-video-pipeline。
//
// 做法:用自带的 ffmpeg 把 HDR 文件离线转成 SDR(mpv 同款的 libplacebo 映射),
// 转完再交给播放器。代价是要重编码(1080p 大约 3~5 倍速),换来的是画面对了。
//
// 三条状态:
//   check()   —— ffprobe 看一眼这个文件是不是 HDR(几百毫秒,异步)
//   convert() —— 转码,带进度
//   dismiss() —— "继续看原片",对当前这个文件不再提示
//
// 输出落在**源文件旁边**:`<原名>_SDR.mkv`。放旁边而不是临时目录,是为了
// 用户下次还能直接用它(而且房间里各人转各自的,谁也不用等谁)。
//

#ifndef SYNCINE_HDRCONVERTER_H
#define SYNCINE_HDRCONVERTER_H

#include <QObject>
#include <QProcess>
#include <QString>
#include <QUrl>

#include <memory>

class Toolchain;

class HdrConverter : public QObject {
    Q_OBJECT

    // 当前检查的文件是不是 HDR(没检查过/不是 → false)
    Q_PROPERTY(bool hdr
               READ hdr
               NOTIFY stateChanged)

    // 该不该弹"要不要转"的提示
    Q_PROPERTY(bool promptVisible
               READ promptVisible
               NOTIFY stateChanged)

    Q_PROPERTY(bool running
               READ running
               NOTIFY stateChanged)

    // 0~100;-1 = 还不知道
    Q_PROPERTY(int progress
               READ progress
               NOTIFY stateChanged)

    // 转出来大概多大(给用户一个"占多少空间"的预期)
    Q_PROPERTY(qint64 estimatedBytes
               READ estimatedBytes
               NOTIFY stateChanged)

    Q_PROPERTY(QString sourcePath
               READ sourcePath
               NOTIFY stateChanged)

    // 转好的文件(QUrl 给播放器/房间用,path 给界面显示)
    Q_PROPERTY(QUrl outputUrl
               READ outputUrl
               NOTIFY stateChanged)
    Q_PROPERTY(QString outputPath
               READ outputPath
               NOTIFY stateChanged)

    Q_PROPERTY(QString errorText
               READ errorText
               NOTIFY stateChanged)

public:
    explicit HdrConverter(const Toolchain *toolchain, QObject *parent = nullptr);
    ~HdrConverter() override;

    bool hdr() const { return m_hdr; }
    bool promptVisible() const;
    bool running() const { return m_convert != nullptr; }
    int progress() const { return m_progress; }
    qint64 estimatedBytes() const;
    QString sourcePath() const { return m_sourcePath; }
    QUrl outputUrl() const;
    QString outputPath() const { return m_outputPath; }
    QString errorText() const { return m_errorText; }

    // 检查一个本地文件(非本地 / 空 → 清空状态)
    Q_INVOKABLE void check(const QUrl &fileUrl);
    Q_INVOKABLE void convert();
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void cancel();

signals:
    void stateChanged();
    // 转好了 —— 调用方负责把它交给播放器(以及房间里的本机文件映射)
    void finished(const QUrl &outputUrl);
    void failed(const QString &message);

private:
    void handleProbeFinished();
    void handleConvertFinished(int exitCode);
    void handleProgressLine(const QString &line);
    void resetForNewSource();
    void fail(const QString &message);
    // 目标码率:跟着源走,但有下限和上限
    int targetBitrateKbps() const;

    const Toolchain *m_toolchain = nullptr;

    QProcess *m_probe = nullptr;     // ffprobe(检查)
    QProcess *m_convert = nullptr;   // ffmpeg(转码)
    QString m_outputTail;            // ffmpeg 的 stderr 尾巴(失败时给用户看)
    QString m_progressBuffer;        // -progress 是流式文本,要按行切开

    QString m_sourcePath;
    QString m_outputPath;
    bool m_hdr = false;
    bool m_dismissed = false;
    int m_progress = -1;
    qint64 m_durationMs = 0;
    qint64 m_sourceBitrateBps = 0;
    QString m_errorText;
};

#endif //SYNCINE_HDRCONVERTER_H
