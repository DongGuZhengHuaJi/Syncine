import QtQuick
import QtQuick.Controls
import QtMultimedia
import SyncineApp

// 播放区(不含下方的弹幕输入条,那块由 RoomPage 拼)。
//
// 画面、弹幕层、控制条三层叠在一起,顺带处理"点画面=播放/暂停"和
// 控制条自动隐藏。形态、音量、麦克风这些状态都不在这里 —— 它们由
// 外部持有,这里只把用户操作转成信号发出去。
Item {
    id: root

    property bool controlsVisible: true

    // 窗口全屏/屏幕全屏时,弹幕输入框并进控制条
    property bool fullscreenStyle: false
    property string viewMode: "normal"
    property bool danmakuVisible: true
    property string hintText: ""

    // 当前模式下还没有可播的片子(本地模式 / 共享模式的房主,且没加载成功)——
    // 这时候点画面不是播放/暂停,而是"去选个文件"
    property bool awaitingFile: false

    signal danmakuSubmitted(string text)
    signal danmakuToggleRequested()
    signal viewModeRequested(string mode)
    signal loadRequested()

    function pushDanmaku(text) {
        danmakuLayer.push(text)
    }

    // 播放源不再由 QML 决定:房间的"当前条目"解析成本地文件 / URL 之后,
    // 由 PlaybackSync 调用 playbackController.load()。这里只管画。

    // 只有一个 VideoOutput —— 它渲染的永远是"界面该显示的那一路"。
    //
    // **不能**写成 videoSink: playbackController.xxx。Qt 里
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
        onClicked: {
            if (root.awaitingFile)
                root.loadRequested()
            else
                playbackSync.togglePlayPause()
        }
        onPositionChanged: showControls()
    }

    // 弹幕层:画面之上、控制条之下
    DanmakuLayer {
        id: danmakuLayer
        anchors.fill: parent
        running: root.danmakuVisible
    }

    // 中心提示(请先加载视频 / 等待其他人加载…),由 RoomPage 决定文案。
    // 往上偏 56px —— 正中间是那个大播放键,两个都居中会叠在一起。
    Rectangle {
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -56
        visible: root.hintText !== ""
        width: hintLabel.implicitWidth + 32
        height: 38
        radius: Style.radius
        color: "#9E000000"

        Text {
            id: hintLabel
            anchors.centerIn: parent
            text: root.hintText
            color: "#FFFFFF"
            font.pixelSize: 13
        }
    }

    // 暂停时中央的大播放键
    Rectangle {
        anchors.centerIn: parent
        width: 64
        height: 64
        radius: 32
        color: "#73000000"
        opacity: playbackSync.playing ? 0 : 1
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        AppIcon {
            anchors.centerIn: parent
            width: 26
            height: 26
            name: "play"
            color: "#FFFFFF"
        }
    }

    PlaybackControl {
        id: playbackControl

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        inlineDanmaku: root.fullscreenStyle
        viewMode: root.viewMode
        danmakuVisible: root.danmakuVisible

        opacity: root.controlsVisible ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 180 } }

        onInteracted: showControls()
        onDanmakuSubmitted: (text) => root.danmakuSubmitted(text)
        onDanmakuToggleRequested: root.danmakuToggleRequested()
        onViewModeRequested: (mode) => root.viewModeRequested(mode)
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
