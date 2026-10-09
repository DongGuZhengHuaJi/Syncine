//
// Created by donggu on 2026/9/6.
//

#ifndef SYNCINE_PLAYBACKCONTROLLER_H
#define SYNCINE_PLAYBACKCONTROLLER_H

#include <QObject>
#include <QUrl>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QAudioBuffer>
#include <QAudioBufferOutput>
#include <QVideoSink>
#include <QVideoFrame>

class RoomSession;
class WebrtcManager;

class PlaybackController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool playing
               READ playing
               NOTIFY playingChanged)

    Q_PROPERTY(bool hasLoaded
               READ hasLoaded
               NOTIFY hasLoadedChanged)

    Q_PROPERTY(bool seekable
               READ seekable
               NOTIFY seekableChanged)

    Q_PROPERTY(qint64 position
               READ position
               NOTIFY positionChanged)

    Q_PROPERTY(qint64 duration
               READ duration
               NOTIFY durationChanged)

    Q_PROPERTY(double volume
               READ volume
               WRITE setVolume
               NOTIFY volumeChanged)

    Q_PROPERTY(bool muted
               READ muted
               WRITE setMuted
               NOTIFY mutedChanged)

    Q_PROPERTY(QMediaPlayer* player
               READ player
               CONSTANT)

    // Qquick 6.4 里 VideoOutput.videoSink 是只读的,只能由 QML 主动交出来
    Q_PROPERTY(QVideoSink* displayVideoSink
               READ displayVideoSink
               NOTIFY displayVideoSinkChanged)

    // 界面该显示哪一路画面。
    // true  → 显示远端(共享模式下房主推过来的)
    // false → 显示本地播放器的画面
    Q_PROPERTY(bool showingRemote
               READ showingRemote
               WRITE setShowingRemote
               NOTIFY showingRemoteChanged)

public:
    explicit PlaybackController(QObject *parent = nullptr);

    bool hasLoaded() const;
    bool playing() const;
    bool seekable() const;
    qint64 position() const;
    qint64 duration() const;

    double volume() const;
    void setVolume(double volume);

    bool muted() const;
    void setMuted(bool muted);

    QString hash() const;

    QMediaPlayer *player() const;
    QVideoSink *displayVideoSink() const;

    bool showingRemote() const;
    void setShowingRemote(bool showing);

    // 远端画面往哪送 —— 就是界面那个 VideoOutput 的 sink。
    // 由 WebrtcManager 的远端渲染器往这里推帧。
    QVideoSink *remoteSinkForRenderer() const {
        return m_outputSink;
    }

    // 由 QML 在 VideoOutput 的 Component.onCompleted 里调用,
    // 把界面那个 sink 交给 C++ 保管。
    //
    // 为什么必须这样做:VideoOutput.videoSink 只读,只能由它主动交出来。
    Q_INVOKABLE void bindVideoOutput(QVideoSink *sink);

    void setRemoteRenderer(WebrtcManager *manager);


    Q_INVOKABLE void load(const QUrl &source);
    Q_INVOKABLE void unload();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlayPause();
    Q_INVOKABLE void seek(qint64 position);
    Q_INVOKABLE void seekRelative(qint64 offset);
    Q_INVOKABLE void toggleMute();

signals:
    void sourceChanged();
    void hasLoadedChanged();
    void showingRemoteChanged();
    void displayVideoSinkChanged();

    // 解码出一帧画面了。推流侧(WebrtcManager)订阅它,把帧转成 WebRTC 格式送出去。
    void videoFrameAvailable(const QVideoFrame &frame);

    // 解码出一帧电影音频了。推流侧订阅它,经 MovieAudioSource 送进 WebRTC。
    //
    // 三个要点:
    //  1. 这是**增益之前**的数据(Qt 直接重采样解码帧,音量是在音频输出上施加的),
    //     所以推送端调自己的音量不会影响观众听到的大小 —— 这正是我们要的。
    //  2. 采样格式固定为 48000Hz / 双声道 / Int16,由 m_audioBufferOutput 构造时
    //     指定,Qt 负责重采样。选 48k 是因为它是 WebRTC Opus 的原生采样率。
    //     立体声能否真正传到观众端,取决于 media 工厂的编解码器工厂有没有给
    //     Opus 协商格式加 stereo=1(见 WebrtcManager.cpp 的两个包装工厂,
    //     缺了解码器那份 sendrecv 列表合并时参数会被丢掉)—— 捕获侧
    //     单方面给 2 声道是不够的。
    //  3. 播放/暂停天然跟随 —— 播放器不出声时就不产生 buffer,
    //     不需要额外的定时器或播放状态判断(这也是用官方 API 而非自行解码的主要好处)。
    void movieAudioFrameAvailable(const QAudioBuffer &buffer);

    // 用户主动跳转(进度条拖动 / ±10s 按钮)。
    void userSeeked(qint64 position);

    void playingChanged();
    void seekableChanged();
    void positionChanged();
    void durationChanged();

    void volumeChanged();
    void mutedChanged();

    void errorOccurred(const QString &message);

private:
    // 解码帧到达。由 bindVideoOutput 接到界面那个 sink 上。
    void onVideoFrame(const QVideoFrame &frame);

    // 解码音频到达。接在 m_audioBufferOutput 上,转成对外信号发出去。
    void onMovieAudioBuffer(const QAudioBuffer &buffer);

    // 根据 showingRemote 决定"谁往界面 sink 送帧"。
    void applyVideoSink();

    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;

    // 从播放器管线旁路出一路解码后的 PCM,用来推流。
    // 它**不影响正常出声** —— m_audioOutput 照常播,这一路只是多拿一份数据。
    //
    // 为什么必须挂着 m_audioOutput:虽然实测不挂也能拿到 PCM
    // (qffmpegplaybackengine.cpp 建渲染器的条件是 m_audioOutput || m_audioBufferOutput),
    // 但挂着房主才听得到声音。想静音请用 m_audioOutput->setMuted(true),
    // **不要** setAudioOutput(nullptr) —— 那会连本地播放一起废掉。
    QAudioBufferOutput *m_audioBufferOutput = nullptr;

    // 界面那个 VideoOutput 的 sink。QML 在 Component.onCompleted 里交过来。
    // 播放器和远端渲染器都往它送 —— 谁在送由 showingRemote 决定。
    QVideoSink *m_outputSink = nullptr;

    // 远端画面往这个管理器的渲染器里送。由 main.cpp 装配时设置。
    // 只用于转发"该往哪个 sink 写"这一条信息 —— 本类不知道 WebRTC 的细节。
    WebrtcManager *m_webrtcManager = nullptr;

    bool m_showingRemote = false;




};

#endif //SYNCINE_PLAYBACKCONTROLLER_H
