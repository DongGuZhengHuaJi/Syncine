//
// 屏幕/窗口共享的采集控制器。
//
// 职责边界：只认识 Qt Multimedia（QScreenCapture/QWindowCapture/QVideoSink），
// 不认识 WebRTC、房间或播放器。采集到的 QVideoFrame 通过 frameAvailable 发出，
// 由 main.cpp 接到 WebrtcManager::pushQtVideoFrame。
//
// 线程：采集回调不一定在 GUI 线程（X11 实测在 GUI，其他平台可能不是）。
// 本类把帧统一搬到 GUI 线程后再发出，并做 30fps 节流；节流在搬运之前，
// 避免 60/120Hz 的采集帧把 GUI 事件队列塞满。
//
// 生命周期：startScreen/startWindow 与 stop 都只在 GUI 线程调用；
// 析构时必须先 stop，再销毁 QMediaCaptureSession 与 capture 对象。
//

#ifndef SYNCINE_SCREENSHARECONTROLLER_H
#define SYNCINE_SCREENSHARECONTROLLER_H

#include <QCapturableWindow>
#include <QList>
#include <QMediaCaptureSession>
#include <QMutex>
#include <QObject>
#include <QScreenCapture>
#include <QString>
#include <QVariantList>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWindowCapture>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

#include "SystemAudioCapture.h"

class QScreen;

class ScreenShareController : public QObject {
    Q_OBJECT

    Q_PROPERTY(bool active READ isActive NOTIFY activeChanged)
    Q_PROPERTY(QString sourceKind READ sourceKind NOTIFY activeChanged)
    Q_PROPERTY(QString sourceLabel READ sourceLabel NOTIFY activeChanged)
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)
    Q_PROPERTY(QVariantList screens READ screens NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantList windows READ windows NOTIFY sourcesChanged)

    // ---- 系统声音 ----
    Q_PROPERTY(bool audioAvailable READ audioAvailable NOTIFY audioAvailableChanged)
    // 用户偏好：下次开始共享时是否采集系统声音；共享中修改会立即启停音频。
    Q_PROPERTY(bool audioEnabled READ audioEnabled WRITE setAudioEnabled
               NOTIFY audioEnabledChanged)
    // 当前是否真的在采集（可能因设备/监视源错误而为 false）。
    Q_PROPERTY(bool audioActive READ audioActive NOTIFY audioActiveChanged)
    Q_PROPERTY(QString audioError READ audioError NOTIFY audioErrorChanged)

public:
    explicit ScreenShareController(QObject *parent = nullptr);
    ~ScreenShareController() override;

    ScreenShareController(const ScreenShareController &) = delete;
    ScreenShareController &operator=(const ScreenShareController &) = delete;

    bool isActive() const {
        return m_active.load(std::memory_order_relaxed);
    }

    // "screen" / "window"；未共享时为 "media"。main.cpp 用它向房间广播。
    QString sourceKind() const {
        return m_sourceKind;
    }

    QString sourceLabel() const {
        return m_sourceLabel;
    }

    QString errorString() const {
        return m_errorString;
    }

    // 供 QML 的选项列表使用。元素形如 { label, value }，
    // value 是索引，传给 startScreen/startWindow。
    QVariantList screens() const {
        return m_screenOptions;
    }
    QVariantList windows() const {
        return m_windowOptions;
    }

    bool audioAvailable() const;
    bool audioEnabled() const {
        return m_audioEnabled;
    }
    Q_INVOKABLE void setAudioEnabled(bool enabled);
    bool audioActive() const {
        return m_audioActive.load(std::memory_order_relaxed);
    }
    QString audioError() const {
        return m_audioError;
    }

    // main.cpp 注入：把系统声音 PCM 交给 MovieAudioSource。
    // 回调在采集线程执行，消费者必须线程安全。
    using AudioSink = std::function<void(const int16_t *data,
                                         size_t samplesPerChannel,
                                         int sampleRate,
                                         size_t channels)>;
    void setAudioSink(AudioSink sink) {
        m_audioSink = std::move(sink);
    }

    // 当前「听筒」设备名（AppSettings.speakerDevice()）。空串 = 系统默认。
    // 音频采集中切换设备会重启采集。
    void setOutputDeviceName(const QString &name);

    // 重新枚举屏幕与窗口。打开共享对话框时调用一次。
    Q_INVOKABLE void refreshSources();

    // 开始共享。返回 false 表示索引无效或采集启动失败；
    // 具体原因通过 errorString / errorOccurred 给出。
    Q_INVOKABLE bool startScreen(int index);
    Q_INVOKABLE bool startWindow(int index);

    // 停止采集。可重复调用。
    Q_INVOKABLE void stop();

signals:
    void activeChanged();
    void errorChanged();
    void sourcesChanged();
    void audioAvailableChanged();
    void audioEnabledChanged();
    void audioActiveChanged();
    void audioErrorChanged();

    // 采集帧。只在 GUI 线程发出，接收方可以直接调用
    // WebrtcManager::pushQtVideoFrame。
    void frameAvailable(const QVideoFrame &frame);

    // 给 main.cpp 打日志用；界面也可以直接读 errorString。
    void errorOccurred(const QString &message);

private:
    void onCapturedFrame(const QVideoFrame &frame);
    void onError(const QString &message);
    void setError(const QString &message);
    void resetFrameState();

    void startAudioCapture();
    void stopAudioCapture();
    void onAudioError(const QString &message);
    void setAudioError(const QString &message);
    void onAudioPcm(const int16_t *data, size_t samplesPerChannel,
                    int sampleRate, size_t channels);

    QMediaCaptureSession m_session;

    // 声明顺序有讲究：session 先于 capture/sink 声明，析构时最后销毁。
    // stop() 里会先把 session 上的指针清空，避免它引用已销毁的对象。
    QScreenCapture m_screenCapture;
    QWindowCapture m_windowCapture;
    QVideoSink m_videoSink;

    QList<QScreen *> m_screens;
    QList<QCapturableWindow> m_windows;
    QVariantList m_screenOptions;
    QVariantList m_windowOptions;

    QString m_sourceKind = QStringLiteral("media");
    QString m_sourceLabel;
    QString m_errorString;

    std::unique_ptr<SystemAudioCapture> m_audioCapture;
    AudioSink m_audioSink;
    bool m_audioAvailable = false;
    bool m_audioEnabled = false;
    std::atomic<bool> m_audioActive{false};
    QString m_audioError;
    QString m_outputDeviceName;

    std::atomic<bool> m_active{false};

    // 采集回调可能来自非 GUI 线程：只保留最新一帧，且同一时刻最多
    // 有一个待处理的 GUI 投递，防止事件队列无界增长。
    mutable QMutex m_frameMutex;
    QVideoFrame m_latestFrame;
    bool m_dispatchPending = false;

    // 节流状态。放在 m_frameMutex 下访问。
    std::chrono::steady_clock::time_point m_clockStart;
    qint64 m_lastAcceptedMs = -1;
    int m_minFrameIntervalMs = 33;
};

#endif //SYNCINE_SCREENSHARECONTROLLER_H
