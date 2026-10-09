import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SyncineApp

Item {
    id: root

    // 本页只做展示和转达用户意图。
    // 播放广播、加载门禁、回声抑制都在 PlaybackSync 里,这一层不再判断。
    property bool leaving: false

    function toggleMic() {
        signalingChannel.setAudioEnabled(!signalingChannel.audioEnabled)
    }

    MessageDialog {
        id: messageDialog
    }


    ListModel {
        id: messageModel
        ListElement { who: "系统"; text: "欢迎来到房间,把房间号发给好友一起观影" }
    }

    function sendMessage() {
        var t = chatInput.text.trim()
        if (t === "")
            return
        messageModel.append({ "who": sessionController.nickname, "text": t })
        chatList.positionViewAtEnd()
        roomSession.sendChat(t)
        chatInput.clear()
    }

    function leaveCurrentRoom() {
        if (leaving)
            return
        leaving = true
        // 先把麦克风关掉再走。否则人已经离开房间了,麦克风还在采集 ——
        // 系统托盘上的录音指示灯会一直亮着,是用户最先注意到的异常。
        signalingChannel.setAudioEnabled(false)
        playbackController.pause()
        sessionController.leaveRoom()
        stackView.pop()
    }

    Connections {
        target: roomSession

        function onChatReceived(from, text) {
            messageModel.append({ "who": from, "text": text })
            chatList.positionViewAtEnd()
        }

        function onMemberJoined(nickname) {
            messageModel.append({ "who": "系统", "text": nickname + " 加入了房间" })
            chatList.positionViewAtEnd()
        }

        function onMemberLeft(nickname) {
            messageModel.append({ "who": "系统", "text": nickname + " 离开了房间" })
            chatList.positionViewAtEnd()
        }

        function onVideoMismatchChanged() {
            if (roomSession.videoMismatched) {
                messageDialog.message = "检测到成员加载的视频不一致,同步将适配较短视频的时长"
                messageDialog.open()
            }
        }

        // 服务端断开(或被动离开)时,弹回上一页
        function onLeft() {
            if (leaving)
                return
            signalingChannel.setAudioEnabled(false)
            playbackController.pause()
            stackView.pop()
        }
    }

    // 麦克风状态不需要 Connections 同步 —— signalingChannel.audioEnabled
    // 是 Q_PROPERTY,引用它的绑定会随 NOTIFY 自动重算。

    FileDialog {
        id: fileDialog
        title: "选择视频文件"
        nameFilters: ["视频文件 (*.mp4 *.mkv *.avi *.mov *.webm *.flv)", "所有文件 (*)"]
        onAccepted: {
            videoPlayer.source = fileDialog.selectedFile
        }
    }

    Rectangle {
        id: topBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 64
        color: Style.card

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Style.border
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 14

            GhostButton {
                text: "← 离开"
                onClicked: root.leaveCurrentRoom()
            }

            Label {
                height: 40
                verticalAlignment: Text.AlignVCenter
                width: Math.min(implicitWidth, 160)
                elide: Text.ElideRight
                text: roomSession.roomName !== "" ? roomSession.roomName
                                                  : ("房间 " + roomSession.roomId)
                font.pixelSize: 17
                font.bold: true
                color: Style.textPrimary
            }

            Item {
                width: statusText.implicitWidth + 22
                height: 40

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: statusText.implicitWidth + 22
                    height: 22
                    radius: 11
                    color: Style.accentSoft

                    Label {
                        id: statusText
                        anchors.centerIn: parent
                        text: roomSession.members.count + " 人在线"
                        font.pixelSize: 11
                        color: Style.accent
                    }
                }
            }
        }

        // 模式选择(本地 / 共享 / 网链)
        Rectangle {
            id: modeSelector
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: 240
            height: 38
            radius: 10
            color: Style.fieldBg
            border.width: 1
            border.color: Style.border

            Row {
                anchors.fill: parent
                anchors.margins: 3
                spacing: 3

                ModeButton {
                    width: 76
                    text: "本地"
                    enabled: roomSession.isHost
                    selected: roomSession.roomMode === RoomSession.Local
                    onClicked: roomSession.setRoomMode(RoomSession.Local)
                }

                ModeButton {
                    width: 76
                    text: "共享"
                    enabled: roomSession.isHost
                    selected: roomSession.roomMode === RoomSession.Share
                    onClicked: roomSession.setRoomMode(RoomSession.Share)
                }

                ModeButton {
                    width: 76
                    text: "网链"
                    enabled: roomSession.isHost
                    selected: roomSession.roomMode === RoomSession.Url
                    onClicked: roomSession.setRoomMode(RoomSession.Url)
                }
            }
        }

        Button {
            id: loadVideoBtn
            anchors.left: modeSelector.right
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter

            width: 96
            height: 36

            contentItem: Label {
                text: "加载视频"
                font.pixelSize: 13
                color: "#FFFFFF"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                radius: 18
                color: loadVideoBtn.hovered ?  "#B30FA3B1" : Style.accent
            }

            onClicked: fileDialog.open()
        }

        // 麦克风开关
        Button {
            id: micBtn
            anchors.left: loadVideoBtn.right
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter

            width: 96
            height: 36

            contentItem: Label {
                text: signalingChannel.audioEnabled ? "关闭语音" : "开启语音"
                font.pixelSize: 13
                color: signalingChannel.audioEnabled ? "#FFFFFF" : Style.textPrimary
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                radius: 18
                // 开麦时用强调色,关麦时用浅色 —— 一眼能看出当前状态,
                // 不用去读文字。这是视频会议类界面的通行做法。
                color: signalingChannel.audioEnabled
                       ? (micBtn.hovered ? Style.accentPressed : Style.accent)
                       : (micBtn.hovered ? Style.fieldHover : Style.fieldBg)
                border.width: signalingChannel.audioEnabled ? 0 : 1
                border.color: Style.border
            }

            onClicked: root.toggleMic()
        }

        // 显示/隐藏侧边栏
        Button {
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            width: 32
            height: 32
            background: Rectangle {
                color: Style.fieldBg
                radius: 16
            }
            contentItem: Label {
                anchors.centerIn: parent
                text: sidebar.visible ? ">>" : "<<"
                font.pixelSize: 18
                color: Style.textPrimary
            }
            onClicked: {
                sidebar.visible = !sidebar.visible
            }
        }
    }

    Row {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: topBar.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 24
        spacing: 24

        Rectangle {
            id: videoArea
            width: sidebar.visible ? parent.width - sidebar.width - 24 : parent.width
            height: parent.height
            radius: Style.radius
            color: "#14171F"
            clip: true

            VideoPlayer {
                id: videoPlayer
                anchors.fill: parent
            }

            Label {
                id: centerHint
                anchors.centerIn: parent
                visible: text !== ""
                font.pixelSize: 15
                color: "#B3FFFFFF"

                text: {
                    if (roomSession.roomMode === RoomSession.Local) {
                        // 门禁状态由 PlaybackSync 判定,这里只负责翻译成文案
                        if (playbackSync.blockReason === PlaybackSync.LocalNotLoaded)
                            return "请先加载视频"
                        if (playbackSync.blockReason === PlaybackSync.OthersNotLoaded)
                            return "等待其他成员加载视频…"
                        return ""
                    }
                    if (roomSession.roomMode === RoomSession.Share)
                        return ""
                    if (roomSession.roomMode === RoomSession.Url)
                        return "网链模式 · 开发中"
                    return ""
                }
            }


        }

        Column {
            id: sidebar
            width: 312
            height: parent.height
            spacing: 20

            // 声音控制:两条音量各自独立。
            //
            // 这两条调的是**你听到的**音量(对端传过来的两路音频),
            // 和顶栏那个麦克风开关(控制你发不发语音)是两回事。
            // 房主那边的电影声和聊天声走的是两条独立的 WebRTC 音轨,
            // 所以能分开调;这也是整个功能最后落在界面上的一步。
            Card {
                id: audioCard
                width: parent.width
                height: 124
                padding: 20

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 6

                    Label {
                        text: "声音"
                        font.pixelSize: 15
                        font.bold: true
                        color: Style.textPrimary
                    }

                    // 电影音量
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Label {
                            Layout.preferredWidth: 34
                            text: "电影"
                            font.pixelSize: 12
                            color: Style.textSecondary
                        }

                        Slider {
                            id: movieVolumeSlider
                            Layout.fillWidth: true
                            Layout.preferredHeight: 22
                            from: 0
                            to: 1
                            // 读的是 SignalingChannel 的属性,状态真身在 WebrtcManager。
                            // onMoved 里**只写对端属性**,不直接给 value 赋值 ——
                            // 那样会打断这条绑定,滑块就再也不同步了。
                            value: signalingChannel.movieVolume
                            onMoved: signalingChannel.movieVolume = value

                            background: Rectangle {
                                x: movieVolumeSlider.leftPadding
                                y: movieVolumeSlider.topPadding + movieVolumeSlider.availableHeight / 2 - height / 2
                                implicitWidth: 100
                                implicitHeight: 4
                                width: movieVolumeSlider.availableWidth
                                height: implicitHeight
                                radius: 2
                                color: Style.border

                                Rectangle {
                                    width: movieVolumeSlider.visualPosition * parent.width
                                    height: parent.height
                                    radius: 2
                                    color: Style.accent
                                }
                            }

                            handle: Rectangle {
                                x: movieVolumeSlider.leftPadding + movieVolumeSlider.visualPosition * (movieVolumeSlider.availableWidth - width)
                                y: movieVolumeSlider.topPadding + movieVolumeSlider.availableHeight / 2 - height / 2
                                implicitWidth: 14
                                implicitHeight: 14
                                radius: 7
                                color: Style.accent
                            }
                        }

                        Label {
                            Layout.preferredWidth: 32
                            horizontalAlignment: Text.AlignRight
                            text: Math.round(movieVolumeSlider.value * 100) + "%"
                            font.pixelSize: 11
                            color: Style.textSecondary
                        }
                    }

                    // 聊天音量
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        Label {
                            Layout.preferredWidth: 34
                            text: "聊天"
                            font.pixelSize: 12
                            color: Style.textSecondary
                        }

                        Slider {
                            id: chatVolumeSlider
                            Layout.fillWidth: true
                            Layout.preferredHeight: 22
                            from: 0
                            to: 1
                            value: signalingChannel.chatVolume
                            onMoved: signalingChannel.chatVolume = value

                            background: Rectangle {
                                x: chatVolumeSlider.leftPadding
                                y: chatVolumeSlider.topPadding + chatVolumeSlider.availableHeight / 2 - height / 2
                                implicitWidth: 100
                                implicitHeight: 4
                                width: chatVolumeSlider.availableWidth
                                height: implicitHeight
                                radius: 2
                                color: Style.border

                                Rectangle {
                                    width: chatVolumeSlider.visualPosition * parent.width
                                    height: parent.height
                                    radius: 2
                                    color: Style.accent
                                }
                            }

                            handle: Rectangle {
                                x: chatVolumeSlider.leftPadding + chatVolumeSlider.visualPosition * (chatVolumeSlider.availableWidth - width)
                                y: chatVolumeSlider.topPadding + chatVolumeSlider.availableHeight / 2 - height / 2
                                implicitWidth: 14
                                implicitHeight: 14
                                radius: 7
                                color: Style.accent
                            }
                        }

                        Label {
                            Layout.preferredWidth: 32
                            horizontalAlignment: Text.AlignRight
                            text: Math.round(chatVolumeSlider.value * 100) + "%"
                            font.pixelSize: 11
                            color: Style.textSecondary
                        }
                    }
                }
            }

            Card {
                id: membersCard
                width: parent.width
                height: 230
                padding: 20

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 14

                    Row {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            height: 24
                            verticalAlignment: Text.AlignVCenter
                            text: "成员"
                            font.pixelSize: 15
                            font.bold: true
                            color: Style.textPrimary
                        }

                        Item {
                            width: memberCountText.implicitWidth + 18
                            height: 24

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: memberCountText.implicitWidth + 18
                                height: 20
                                radius: 10
                                color: Style.fieldBg

                                Label {
                                    id: memberCountText
                                    anchors.centerIn: parent
                                    text: roomSession.members.count + " 人"
                                    font.pixelSize: 11
                                    color: Style.textSecondary
                                }
                            }
                        }
                    }

                    ListView {
                        id: memberList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 12
                        model: roomSession.members

                        delegate: Row {
                            width: memberList.width
                            spacing: 12

                            Rectangle {
                                width: 38
                                height: 38
                                radius: 19
                                color: isHost ? Style.accent : Style.accentSoft

                                Label {
                                    anchors.centerIn: parent
                                    text: nickname.length > 0
                                          ? nickname.charAt(0) : "?"
                                    font.pixelSize: 15
                                    font.bold: true
                                    color: isHost ? "#FFFFFF" : Style.accent
                                }
                            }

                            Column {
                                spacing: 4

                                Label {
                                    text: nickname
                                    font.pixelSize: 14
                                    color: Style.textPrimary
                                }

                                Label {
                                    text: (isHost ? "房主" : "成员")
                                          + (clientId === roomSession.clientId ? " · 你" : "")
                                          + (!loaded && roomSession.roomMode === RoomSession.Local
                                             ? " · 未加载" : "")
                                    font.pixelSize: 11
                                    color: (!loaded && roomSession.roomMode === RoomSession.Local)
                                           ? Style.danger : Style.textSecondary
                                }
                            }
                        }
                    }

                    Row {
                        Layout.fillWidth: true
                        spacing: 6

                        Label {
                            text: "房间号"
                            font.pixelSize: 12
                            color: Style.textSecondary
                        }

                        Label {
                            text: roomSession.roomId
                            font.pixelSize: 13
                            font.bold: true
                            color: Style.accent
                        }
                    }
                }
            }

            Card {
                width: parent.width
                // 三张卡片之间有两道 spacing(20),所以减 40
                height: parent.height - audioCard.height - membersCard.height - 40
                padding: 20

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    Label {
                        text: "聊天"
                        font.pixelSize: 15
                        font.bold: true
                        color: Style.textPrimary
                    }

                    ListView {
                        id: chatList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 10
                        clip: true
                        model: messageModel

                        delegate: Rectangle {
                            width: chatList.width
                            height: bubbleText.implicitHeight + 20
                            radius: 10
                            color: model.who === "系统" ? Style.fieldBg : Style.accentSoft

                            Label {
                                id: bubbleText
                                anchors.fill: parent
                                anchors.margins: 10
                                text: model.who === "系统" ? model.text : model.who + ": " + model.text
                                wrapMode: Text.Wrap
                                font.pixelSize: 13
                                color: Style.textPrimary
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10

                        AppTextField {
                            id: chatInput
                            Layout.fillWidth: true
                            placeholderText: "说点什么…"
                            onAccepted: root.sendMessage()
                        }

                        PrimaryButton {
                            Layout.preferredWidth: 68
                            Layout.preferredHeight: Style.inputHeight
                            text: "发送"
                            onClicked: root.sendMessage()
                        }
                    }
                }
            }
        }
    }
}
