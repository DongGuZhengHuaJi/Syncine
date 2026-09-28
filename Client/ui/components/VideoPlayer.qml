import QtQuick
import QtQuick.Controls
import QtMultimedia

Item {
    id: root

    property url source: ""
    property bool controlsVisible: true

    // 用户的跳转意图不再往上报:PlaybackController 自己会发 userSeeked,
    // 由 PlaybackSync 统一决定要不要广播给房间
    onSourceChanged: playbackController.load(source)

    // 只有一个 VideoOutput —— 它渲染的永远是"界面该显示的那一路"。
    //
    // **不能**写成 videoSink: playbackController.xxx。Qt 6.4 里
    // VideoOutput.videoSink 是只读属性,连绑定赋值都报
    // "Invalid property assignment: videoSink is a read-only property"。
    //
    // 所以方向反过来:把我们的 sink 交给 C++,由 C++ 决定谁往里面送帧
    // (本地播放器 或 远端渲染器)。
    VideoOutput {
        id: videoOutput
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit

        Component.onCompleted: playbackController.bindVideoOutput(videoOutput.videoSink)
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onClicked: playbackSync.togglePlayPause()
        onPositionChanged: showControls()
    }

    Rectangle {
        anchors.centerIn: parent
        width: 80
        height: 80
        radius: 40
        color: "#73000000"
        opacity: playbackSync.playing ? 0 : 1
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
            if (playbackSync.playing)
                root.controlsVisible = false
        }
    }

    Connections {
        target: playbackSync
        function onPlayingChanged() {
            if (!playbackSync.playing)
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
