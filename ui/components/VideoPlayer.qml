import QtQuick
import QtQuick.Controls
import QtMultimedia

Item {
    id: root

    property url source: ""
    property bool controlsVisible: true

    MediaPlayer {
        id: mediaPlayer

        source: root.source
        videoOutput: videoOutput
        audioOutput: audioOutput

        onMediaStatusChanged: {
            if (mediaStatus === MediaPlayer.LoadedMedia) {
                console.log(
                    "Media loaded, duration:",
                    duration,
                    "ms"
                )
            }
        }

        onErrorOccurred: {
            console.log("Media error:", errorString)
        }

        onPlaybackStateChanged: {
            if (playbackState !== MediaPlayer.PlayingState)
                showControls()
            else
                hideTimer.restart()
        }
    }

    VideoOutput {
        id: videoOutput
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit
    }

    AudioOutput {
        id: audioOutput
        volume: 0.6
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: togglePlayback()
        onPositionChanged: showControls()
    }

    Rectangle {
        anchors.centerIn: parent
        width: 80
        height: 80
        radius: 40
        color: "#73000000"
        opacity: mediaPlayer.playbackState !== MediaPlayer.PlayingState ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Canvas {
            anchors.centerIn: parent
            width: 30
            height: 34
            onPaint: {
                var ctx = getContext("2d")
                ctx.fillStyle = "#FFFFFF"
                ctx.beginPath()
                ctx.moveTo(3, 0)
                ctx.lineTo(30, 17)
                ctx.lineTo(3, 34)
                ctx.closePath()
                ctx.fill()
            }
        }
    }

    PlaybackControl {
        id: playbackControl

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        mediaPlayer: mediaPlayer
        audioOutput: audioOutput

        opacity: root.controlsVisible ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }

        onInteracted: showControls()
    }

    Timer {
        id: hideTimer
        interval: 3000
        onTriggered: {
            if (mediaPlayer.playbackState === MediaPlayer.PlayingState)
                root.controlsVisible = false
        }
    }

    function togglePlayback() {
        if (mediaPlayer.playbackState === MediaPlayer.PlayingState)
            mediaPlayer.pause()
        else
            mediaPlayer.play()
    }

    function showControls() {
        root.controlsVisible = true
        hideTimer.restart()
    }
}
