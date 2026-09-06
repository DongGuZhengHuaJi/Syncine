import QtQuick
import QtQuick.Controls
import QtMultimedia

Item {
    id: root

    property url source: ""
    property bool controlsVisible: true

    onSourceChanged: playbackController.load(source)

    VideoOutput {
        id: videoOutput
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit

        Component.onCompleted: playbackController.player.videoOutput = videoOutput
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: playbackController.togglePlayPause()
        onPositionChanged: showControls()
    }

    Rectangle {
        anchors.centerIn: parent
        width: 80
        height: 80
        radius: 40
        color: "#73000000"
        opacity: playbackController.playing ? 0 : 1
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

        opacity: root.controlsVisible ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }

        onInteracted: showControls()
    }

    Timer {
        id: hideTimer
        interval: 3000
        onTriggered: {
            if (playbackController.playing)
                root.controlsVisible = false
        }
    }

    Connections {
        target: playbackController
        function onPlayingChanged() {
            if (!playbackController.playing)
                showControls()
            else
                hideTimer.restart()
        }
    }

    function showControls() {
        root.controlsVisible = true
        hideTimer.restart()
    }
}
