import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SyncineApp

// 播放器控制区,两行:
//   第一行:通栏进度条(和播放器等宽)
//   第二行:按钮 —— 播放 / ±10s / 时间 / [弹幕输入框] / 画质 / 弹幕开关 /
//                    电影音量 / 语音音量 / 麦克风 / 宽屏 / 窗口全屏 / 屏幕全屏
//
// 本组件不存任何状态:形态、弹幕开关、音量、画质真身都在外部
// (RoomPage / SignalingChannel),这里只用信号把用户意图转达出去。
Rectangle {
    id: root

    // 共享模式下的推送画质可选项。索引即 WebrtcManager::VideoQuality 的值
    readonly property var videoQualities: ["流畅", "标准", "高清", "原画"]

    signal interacted()
    signal danmakuSubmitted(string text)
    signal danmakuToggleRequested()
    signal viewModeRequested(string mode)

    // 窗口全屏/屏幕全屏时,弹幕输入框并进这一行(非全屏时它在播放器下方)
    property bool inlineDanmaku: false
    property string viewMode: "normal"
    property bool danmakuVisible: true

    implicitHeight: 74
    color: Style.playerBar

    // 画质选择浮层:贴在画质按钮正上方
    Popup {
        id: qualityPopup

        // **坐标基准必须是按钮本身**。Popup 不设 parent 时以"窗口内容项"为基准,
        // 和按钮的实际位置对不上 —— 浮层会跑到别处,连点击落点也跟着错。
        // 挂在按钮上之后,x/y 就是最朴素的属性读,不依赖 mapToItem 那种函数式绑定。
        parent: qualityBtn

        width: 96
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        // 右边缘和按钮对齐、底边贴在按钮上方 8px(负的 y 没问题:
        // Popup 画在窗口浮层上,不会被父项裁剪)
        x: qualityBtn.width - width
        y: -height - 8

        background: Rectangle {
            radius: Style.radius
            color: "#E6161922"
            border.width: 1
            border.color: Style.playerLine
        }

        contentItem: Column {
            spacing: 2

            Repeater {
                model: root.videoQualities

                delegate: Rectangle {
                    width: qualityPopup.width
                    height: 30
                    radius: Style.radiusSmall
                    color: index === signalingChannel.videoQuality
                           ? "#2E00AEEC"
                           : (qualityRow.containsMouse ? "#29FFFFFF" : "transparent")

                    Text {
                        anchors.centerIn: parent
                        text: root.videoQualities[index]
                        font.pixelSize: 13
                        color: index === signalingChannel.videoQuality
                               ? Style.accent : Style.playerIcon
                    }

                    MouseArea {
                        id: qualityRow
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            signalingChannel.setVideoQuality(index)
                            qualityPopup.close()
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Style.playerLine
    }

    // 鼠标在控制区上动一下就重置"自动隐藏"的计时
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onPositionChanged: root.interacted()
    }

    HoverHandler {
        onHoveredChanged: if (hovered) root.interacted()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        anchors.topMargin: 7
        anchors.bottomMargin: 8
        spacing: 3

        PlaybackSeekControl {
            Layout.fillWidth: true
            onInteracted: root.interacted()
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 2

            // ── 播放 ────────────────────────────────
            ControlButton {
                iconName: playbackSync.playing ? "pause" : "play"
                tip: playbackSync.playing ? "暂停" : "播放"
                onClicked: {
                    playbackSync.togglePlayPause()
                    root.interacted()
                }
            }

            ControlButton {
                text: "-10"
                tip: "后退 10 秒"
                onClicked: {
                    playbackSync.seekRelative(-10000)
                    root.interacted()
                }
            }

            ControlButton {
                text: "+10"
                tip: "前进 10 秒"
                onClicked: {
                    playbackSync.seekRelative(10000)
                    root.interacted()
                }
            }

            Row {
                Layout.leftMargin: 6
                Layout.rightMargin: 4
                spacing: 2

                Text {
                    text: Style.formatTime(playbackSync.position)
                    color: Style.playerIcon
                    font.pixelSize: 12
                }

                Text {
                    text: "/"
                    color: Style.playerIconDim
                    font.pixelSize: 12
                }

                Text {
                    text: Style.formatTime(playbackSync.duration)
                    color: Style.playerIconDim
                    font.pixelSize: 12
                }
            }

            // ── 全屏时的弹幕输入框 ───────────────────
            RowLayout {
                visible: root.inlineDanmaku
                Layout.leftMargin: 8
                Layout.fillWidth: true
                Layout.maximumWidth: 420
                spacing: 6

                TextField {
                    id: inlineDanmakuField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    leftPadding: 10
                    rightPadding: 10
                    font.pixelSize: 12
                    color: Style.playerIcon
                    placeholderText: "发个友善的弹幕见证当下"
                    placeholderTextColor: "#8AFFFFFF"
                    selectByMouse: true
                    background: Rectangle {
                        radius: Style.radiusSmall
                        color: Style.playerField
                        border.width: 1
                        border.color: inlineDanmakuField.activeFocus ? Style.accent
                                                                    : "#33FFFFFF"
                    }
                    Keys.onReturnPressed: submitInline()
                    function submitInline() {
                        const t = text.trim()
                        if (t === "")
                            return
                        root.danmakuSubmitted(t)
                        text = ""
                    }
                }

                Button {
                    id: inlineSendBtn
                    Layout.preferredWidth: 56
                    Layout.preferredHeight: 28
                    hoverEnabled: true
                    contentItem: Text {
                        text: "发送"
                        font.pixelSize: 12
                        color: Style.textOnAccent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: Style.radiusSmall
                        color: inlineSendBtn.pressed ? Style.accentPressed
                             : (inlineSendBtn.hovered ? Style.accentHover : Style.accent)
                    }
                    onClicked: inlineDanmakuField.submitInline()
                }
            }

            Item { Layout.fillWidth: true }

            // ── 推送画质 ────────────────────────────
            // 只有共享模式下的房主在推流,所以只有他能设。
            // 观众看不到这个按钮 —— 他们看到的画质由房主决定。
            ControlButton {
                id: qualityBtn
                visible: roomSession.roomMode === RoomSession.Share && roomSession.isHost
                // 切出共享模式时按钮会藏起来,浮层要跟着收掉,否则会留在屏幕上
                onVisibleChanged: if (!visible) qualityPopup.close()
                // 纯属性读(不要写成函数调用):绑定要跟着 videoQualityChanged 走
                text: root.videoQualities[signalingChannel.videoQuality]
                tip: "推送画质"
                onClicked: qualityPopup.opened ? qualityPopup.close() : qualityPopup.open()
            }

            // ── 弹幕开关 ────────────────────────────
            Button {
                id: danmakuBtn
                Layout.preferredWidth: 26
                Layout.preferredHeight: 26
                implicitWidth: 26
                implicitHeight: 26
                hoverEnabled: true

                contentItem: Text {
                    text: "弹"
                    font.pixelSize: 12
                    font.bold: true
                    color: root.danmakuVisible ? Style.accent : Style.playerIconDim
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: Style.radiusSmall
                    color: root.danmakuVisible ? "#2E00AEEC" : "transparent"
                    border.width: 1
                    border.color: root.danmakuVisible
                                  ? Style.accent
                                  : (danmakuBtn.hovered ? "#FFFFFF" : "#73FFFFFF")
                }

                ToolTip.visible: hovered
                ToolTip.text: root.danmakuVisible ? "关闭弹幕" : "开启弹幕"
                ToolTip.delay: 500

                onClicked: root.danmakuToggleRequested()
            }

            Item { Layout.preferredWidth: 6 }

            // ── 两路音量,各自一个按钮 ───────────────
            VolumeButton {
                icon: "speaker"
                tip: "电影音量"
                value: signalingChannel.movieVolume
                onMoved: signalingChannel.movieVolume = v
                onInteracted: root.interacted()
            }

            VolumeButton {
                icon: "headset"
                tip: "语音音量"
                value: signalingChannel.chatVolume
                onMoved: signalingChannel.chatVolume = v
                onInteracted: root.interacted()
            }

            Item { Layout.preferredWidth: 6 }

            // ── 麦克风(只控制你自己发不发语音) ───────
            ControlButton {
                iconName: signalingChannel.audioEnabled ? "mic" : "mic-off"
                active: signalingChannel.audioEnabled
                tip: signalingChannel.audioEnabled ? "关闭语音" : "开启语音"
                onClicked: {
                    signalingChannel.setAudioEnabled(!signalingChannel.audioEnabled)
                    root.interacted()
                }
            }

            Item { Layout.preferredWidth: 6 }

            // ── 三个形态按钮,各管各的 ─────────────────
            ControlButton {
                iconName: "theater"
                active: root.viewMode === "theater"
                tip: "宽屏模式"
                onClicked: root.viewModeRequested(root.viewMode === "theater"
                                                  ? "normal" : "theater")
            }

            ControlButton {
                iconName: "window-full"
                active: root.viewMode === "winfull"
                tip: "窗口全屏"
                onClicked: root.viewModeRequested(root.viewMode === "winfull"
                                                  ? "normal" : "winfull")
            }

            ControlButton {
                iconName: "screen-full"
                active: root.viewMode === "screenfull"
                tip: "屏幕全屏"
                onClicked: root.viewModeRequested(root.viewMode === "screenfull"
                                                  ? "normal" : "screenfull")
            }
        }
    }
}
