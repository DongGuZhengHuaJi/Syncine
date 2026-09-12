//
// Created by donggu on 2026/9/6.
//

#ifndef SYNCINE_PLAYBACKCONTROLLER_H
#define SYNCINE_PLAYBACKCONTROLLER_H

#include <QObject>
#include <QUrl>
#include <QMediaPlayer>
#include <QAudioOutput>

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

    void playingChanged();
    void seekableChanged();
    void positionChanged();
    void durationChanged();

    void volumeChanged();
    void mutedChanged();

    void errorOccurred(const QString &message);

private:
    QMediaPlayer *m_player;
    QAudioOutput *m_audioOutput;
};

#endif //SYNCINE_PLAYBACKCONTROLLER_H
