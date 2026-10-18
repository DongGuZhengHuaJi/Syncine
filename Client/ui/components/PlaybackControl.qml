import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SyncineApp

// 播放器控制区，两行：
//   第一行：通栏进度条（和播放器等宽）
//   第二行：按钮 —— 播放 / ±10s / 时间 / [弹幕输入框] / 画质 / 弹幕开关 /
//                    电影音量 / 语音音量 / 麦克风 / 宽屏 / 窗口全屏 / 屏幕全屏
//
// 本组件不存任何状态：形态、弹幕开关、音量、画质真身都在外部
// (RoomPage / SignalingChannel),这里只用信号把用户意图转达出去。
Rectangle {
    id: root

    // 共享模式下的推送画质可选项。索引即 WebrtcManager::VideoQuality 的值
    readonly property var videoQualities: ["流畅", "标准", "高清", "原画"]

    // 倍速档位。共享模式与直播不提供（原因见 playbackSync.setPlaybackRate）
    readonly property var playbackRates: [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]

    // 弹幕设置的五个档位
    readonly property var danmakuAreas: [10, 25, 50, 75, 100]
    readonly property var danmakuSpeedLabels: ["0.5x", "0.75x", "1.0x", "1.25x", "1.5x"]

    function rateLabel(rate) {
        // 1.0 → "1.0x"，0.75 → "0.75x"
        return (Math.abs(rate - Math.round(rate)) < 0.001
                ? rate.toFixed(1)
                : String(rate)) + "x"
    }

    signal interacted()
    signal danmakuSubmitted(string text)
    signal danmakuToggleRequested()
    signal viewModeRequested(string mode)
    // 齿轮（= 声音设置）。弹窗本身声明在 RoomPage 根上，这里只发意图 ——
    // 理由见 AudioSettingsDialog 顶部
    signal audioSettingsRequested()

    // 窗口全屏/屏幕全屏时，弹幕输入框并进这一行（非全屏时它在播放器下方）
    property bool inlineDanmaku: false
    property string viewMode: "normal"
    property bool danmakuVisible: true

    implicitHeight: 74
    color: Style.playerBar

    // 画质选择浮层：贴在画质按钮正上方
    Popup {
        id: qualityPopup

        // **坐标基准必须是按钮本身**。Popup 不设 parent 时以"窗口内容项"为基准，
        // 和按钮的实际位置对不上 —— 浮层会跑到别处，连点击落点也跟着错。
        // 挂在按钮上之后，x/y 就是最朴素的属性读，不依赖 mapToItem 那种函数式绑定。
        parent: qualityBtn

        width: 96
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        // 右边缘和按钮对齐、底边贴在按钮上方 8px(负的 y 没问题：
        // Popup 画在窗口浮层上，不会被父项裁剪)
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

    // 倍速选择浮层：和画质浮层同一套做法（贴在按钮正上方）
    Popup {
        id: ratePopup

        parent: rateBtn

        width: 96
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        x: rateBtn.width - width
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
                model: root.playbackRates

                delegate: Rectangle {
                    readonly property bool current:
                        Math.abs(root.playbackRates[index]
                                 - playbackSync.playbackRate) < 0.001

                    width: ratePopup.width
                    height: 30
                    radius: Style.radiusSmall
                    color: current ? "#2E00AEEC"
                                   : (rateRow.containsMouse ? "#29FFFFFF" : "transparent")

                    Text {
                        anchors.centerIn: parent
                        text: root.rateLabel(root.playbackRates[index])
                        font.pixelSize: 13
                        color: current ? Style.accent : Style.playerIcon
                    }

                    MouseArea {
                        id: rateRow
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            playbackSync.setPlaybackRate(root.playbackRates[index])
                            ratePopup.close()
                        }
                    }
                }
            }
        }
    }

    // ── 弹幕设置的小控件 ─────────────────────────
    component ChoiceChip: Rectangle {
        id: chip

        property string label: ""
        property bool current: false
        signal picked()

        implicitWidth: Math.max(34, chipLabel.implicitWidth + 12)
        implicitHeight: 24
        radius: Style.radiusSmall
        border.width: 1
        color: chip.current ? "#2E00AEEC"
                            : (chipMouse.containsMouse ? "#29FFFFFF" : "transparent")
        border.color: chip.current ? Style.accent : "#33FFFFFF"

        Text {
            id: chipLabel
            anchors.centerIn: parent
            text: chip.label
            font.pixelSize: 11
            color: chip.current ? Style.accent : Style.playerIcon
        }

        MouseArea {
            id: chipMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: chip.picked()
        }
    }

    component PlayerSlider: Slider {
        id: control

        implicitHeight: 22

        background: Rectangle {
            x: control.leftPadding
            y: control.topPadding + control.availableHeight / 2 - height / 2
            implicitWidth: 100
            implicitHeight: 3
            width: control.availableWidth
            height: implicitHeight
            color: Style.playerTrack

            Rectangle {
                width: control.visualPosition * parent.width
                height: parent.height
                color: Style.accent
            }
        }

        handle: Rectangle {
            x: control.leftPadding + control.visualPosition
               * (control.availableWidth - width)
            y: control.topPadding + control.availableHeight / 2 - height / 2
            implicitWidth: 10
            implicitHeight: 10
            radius: 5
            color: "#FFFFFF"
        }
    }

    component PlayerSwitch: Item {
        id: toggle

        property bool checked: false
        signal toggled()

        implicitWidth: 40
        implicitHeight: 22

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: toggle.checked ? Style.accent : "#3DFFFFFF"
            border.width: 1
            border.color: toggle.checked ? Style.accent : "#33FFFFFF"

            Rectangle {
                x: toggle.checked ? parent.width - width - 2 : 2
                y: 2
                width: 16
                height: 16
                radius: 8
                color: "#FFFFFF"
                Behavior on x {
                    NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: toggle.toggled()
        }
    }

    // 弹幕设置浮层。入口是「弹」开关右侧的小三角。
    Popup {
        id: danmakuSettingsPopup

        parent: danmakuSettingsBtn

        width: 286
        padding: 14
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        x: danmakuSettingsBtn.width - width
        y: -height - 8

        background: Rectangle {
            radius: Style.radius
            color: "#E6161922"
            border.width: 1
            border.color: Style.playerLine
        }

        contentItem: ColumnLayout {
            width: danmakuSettingsPopup.availableWidth
            spacing: 10

            Text {
                text: "弹幕设置"
                color: Style.playerIcon
                font.pixelSize: 13
                font.bold: true
            }

            // 显示区域
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Text {
                    text: "显示区域"
                    color: Style.playerIconDim
                    font.pixelSize: 11
                }

                Row {
                    spacing: 4

                    Repeater {
                        model: root.danmakuAreas

                        delegate: ChoiceChip {
                            required property var modelData
                            label: modelData + "%"
                            current: appSettings.danmakuArea === modelData
                            onPicked: appSettings.danmakuArea = modelData
                        }
                    }
                }
            }

            // 不透明度
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true

                    Text {
                        text: "不透明度"
                        color: Style.playerIconDim
                        font.pixelSize: 11
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: Math.round(appSettings.danmakuOpacity) + "%"
                        color: Style.playerIcon
                        font.pixelSize: 11
                    }
                }

                PlayerSlider {
                    Layout.fillWidth: true
                    from: 10
                    to: 100
                    stepSize: 5
                    value: appSettings.danmakuOpacity
                    onMoved: appSettings.danmakuOpacity = Math.round(value)
                }
            }

            // 字号
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true

                    Text {
                        text: "字号"
                        color: Style.playerIconDim
                        font.pixelSize: 11
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: Math.round(appSettings.danmakuFontSize) + "%"
                        color: Style.playerIcon
                        font.pixelSize: 11
                    }
                }

                PlayerSlider {
                    Layout.fillWidth: true
                    from: 50
                    to: 200
                    stepSize: 10
                    value: appSettings.danmakuFontSize
                    onMoved: appSettings.danmakuFontSize = Math.round(value)
                }
            }

            // 速度
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Text {
                    text: "速度"
                    color: Style.playerIconDim
                    font.pixelSize: 11
                }

                Row {
                    spacing: 4

                    Repeater {
                        model: root.danmakuSpeedLabels

                        delegate: ChoiceChip {
                            required property var modelData
                            required property int index
                            label: modelData
                            current: appSettings.danmakuSpeed === index
                            onPicked: appSettings.danmakuSpeed = index
                        }
                    }
                }
            }

            // 随屏幕缩放
            RowLayout {
                Layout.fillWidth: true

                Text {
                    text: "随屏幕缩放"
                    color: Style.playerIconDim
                    font.pixelSize: 11
                }

                Item { Layout.fillWidth: true }

                PlayerSwitch {
                    checked: appSettings.danmakuScaleWithScreen
                    onToggled: appSettings.danmakuScaleWithScreen =
                               !appSettings.danmakuScaleWithScreen
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

                // 直播：没有总时长，也没有可拖的时间轴 —— 直接标出来
                Text {
                    visible: playbackSync.live
                    anchors.verticalCenter: parent.verticalCenter
                    text: "● 直播"
                    color: Style.accent
                    font.pixelSize: 12
                }

                Text {
                    visible: !playbackSync.live
                    text: Style.formatTime(playbackSync.position)
                    color: Style.playerIcon
                    font.pixelSize: 12
                }

                Text {
                    visible: !playbackSync.live
                    text: "/"
                    color: Style.playerIconDim
                    font.pixelSize: 12
                }

                Text {
                    visible: !playbackSync.live
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

            // ── 播放速度 ────────────────────────────
            // 共享模式下禁用：电影音频按实时节奏推流，换挡会让观众端断续；
            // 直播也禁用：没有共同时间轴，播放器对直播换挡的支持不可靠。
            ControlButton {
                id: rateBtn
                visible: !playbackSync.live
                         && roomSession.roomMode !== RoomSession.Share
                onVisibleChanged: if (!visible) ratePopup.close()
                text: root.rateLabel(playbackSync.playbackRate)
                tip: "播放速度"
                active: Math.abs(playbackSync.playbackRate - 1.0) > 0.001
                onClicked: ratePopup.opened ? ratePopup.close() : ratePopup.open()
            }

            // ── 推送画质 ────────────────────────────
            // 只有共享模式下的房主在推流，所以只有他能设。
            // 观众看不到这个按钮 —— 他们看到的画质由房主决定。
            ControlButton {
                id: qualityBtn
                visible: roomSession.roomMode === RoomSession.Share && roomSession.isHost
                // 切出共享模式时按钮会藏起来，浮层要跟着收掉，否则会留在屏幕上
                onVisibleChanged: if (!visible) qualityPopup.close()
                // 纯属性读（不要写成函数调用）:绑定要跟着 videoQualityChanged 走
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

            // 弹幕设置入口：紧挨着「弹」开关
            ControlButton {
                id: danmakuSettingsBtn
                implicitWidth: 20
                Layout.preferredWidth: 20
                text: "▾"
                tip: "弹幕设置"
                onClicked: danmakuSettingsPopup.opened
                           ? danmakuSettingsPopup.close()
                           : danmakuSettingsPopup.open()
            }

            Item { Layout.preferredWidth: 6 }

            // ── 两路音量，各自一个按钮 ───────────────
            VolumeButton {
                icon: "speaker"
                tip: "电影音量"
                value: signalingChannel.movieVolume
                onMoved: (v) => { signalingChannel.movieVolume = v }
                onInteracted: root.interacted()
            }

            VolumeButton {
                icon: "headset"
                tip: "语音音量"
                value: signalingChannel.chatVolume
                onMoved: (v) => { signalingChannel.chatVolume = v }
                onInteracted: root.interacted()
            }

            Item { Layout.preferredWidth: 6 }

            // ── 麦克风（仅控制本端是否发送语音） ───────
            ControlButton {
                iconName: signalingChannel.audioEnabled ? "mic" : "mic-off"
                active: signalingChannel.audioEnabled
                tip: signalingChannel.audioEnabled ? "关闭语音" : "开启语音"
                onClicked: {
                    signalingChannel.setAudioEnabled(!signalingChannel.audioEnabled)
                    root.interacted()
                }
            }

            // ── 声音设置（麦克风 / 听筒设备） ─────────
            // 放在控制条上而不是顶栏：顶栏在两种全屏模式下都会收起，
            // 而"耳机插上了声音还在外放"往往是全屏看片时才发现的
            ControlButton {
                iconName: "gear"
                tip: "声音设置"
                onClicked: {
                    root.audioSettingsRequested()
                    root.interacted()
                }
            }

            Item { Layout.preferredWidth: 6 }

            // ── 三个形态按钮，各管各的 ─────────────────
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
