import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SyncineApp

// 播放器控制区,两行:
//   第一行:通栏进度条(和播放器等宽)
//   第二行:按钮 —— 播放 / ±10s / 时间 / [弹幕输入框] / 弹幕开关 /
//                    电影音量 / 语音音量 / 麦克风 / 宽屏 / 窗口全屏 / 屏幕全屏
//
// 本组件不存任何状态:形态、弹幕开关、音量真身都在外部(RoomPage / SignalingChannel),
// 这里只用信号把用户意图转达出去。
Rectangle {
    id: root

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
