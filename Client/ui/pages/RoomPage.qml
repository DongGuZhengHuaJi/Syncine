import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SyncineApp

Item {
    id: root

    property string roomMode: "local"

    // 正在执行远端命令时为 true,期间不广播自己的播放状态,避免回声
    property bool remoteApplying: false
    property bool leaving: false

    ListModel {
        id: messageModel
        ListElement { who: "系统"; text: "欢迎来到房间,把房间号发给好友一起看片" }
    }

    function sendMessage() {
        var t = chatInput.text.trim()
        if (t === "")
            return
        messageModel.append({ "who": roomManager.nickname, "text": t })
        chatList.positionViewAtEnd()
        roomManager.sendChat(t)
        chatInput.clear()
    }

    function leaveCurrentRoom() {
        if (leaving)
            return
        leaving = true
        playbackController.pause()
        roomManager.leaveRoom()
        stackView.pop()
    }

    Connections {
        target: roomManager

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

        function onPlaybackReceived(action, position) {
            remoteApplying = true
            playbackController.seek(position)
            if (action === "play")
                playbackController.play()
            else if (action === "pause")
                playbackController.pause()
            remoteApplying = false
        }

        // 服务端断开(或被动离开)时,弹回上一页
        function onRoomLeft() {
            if (leaving)
                return
            playbackController.pause()
            stackView.pop()
        }
    }

    Connections {
        target: playbackController

        // 只有用户自己的播放/暂停才广播;远端命令带 remoteApplying 标志
        function onPlayingChanged() {
            if (remoteApplying)
                return
            roomManager.sendPlayback(playbackController.playing ? "play" : "pause",
                                     playbackController.position)
        }
    }

    Connections {
        target: videoPlayer

        function onUserSeeked(position) {
            roomManager.sendPlayback("seek", position)
        }
    }

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
                text: roomManager.roomName !== "" ? roomManager.roomName
                                                  : ("房间 " + roomManager.roomId)
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
                        text: roomManager.members.length + " 人在线"
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
                    selected: root.roomMode === "local"
                    onClicked: root.roomMode = "local"
                }

                ModeButton {
                    width: 76
                    text: "共享"
                    selected: root.roomMode === "share"
                    onClicked: root.roomMode = "share"
                }

                ModeButton {
                    width: 76
                    text: "网链"
                    selected: root.roomMode === "url"
                    onClicked: root.roomMode = "url"
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
                color: loadVideoBtn.hovered ? Style.accent : "#B30FA3B1"
            }

            onClicked: fileDialog.open()
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
                anchors.centerIn: parent
                visible: root.roomMode !== "local"
                text: root.roomMode === "share" ? "共享模式 · 开发中" : "网链模式 · 开发中"
                font.pixelSize: 15
                color: "#B3FFFFFF"
            }


        }

        Column {
            id: sidebar
            width: 312
            height: parent.height
            spacing: 20

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
                                    text: roomManager.members.length + " 人"
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
                        model: roomManager.members

                        delegate: Row {
                            width: memberList.width
                            spacing: 12

                            Rectangle {
                                width: 38
                                height: 38
                                radius: 19
                                color: modelData.isHost ? Style.accent : Style.accentSoft

                                Label {
                                    anchors.centerIn: parent
                                    text: modelData.nickname.length > 0
                                          ? modelData.nickname.charAt(0) : "?"
                                    font.pixelSize: 15
                                    font.bold: true
                                    color: modelData.isHost ? "#FFFFFF" : Style.accent
                                }
                            }

                            Column {
                                spacing: 4

                                Label {
                                    text: modelData.nickname
                                    font.pixelSize: 14
                                    color: Style.textPrimary
                                }

                                Label {
                                    text: (modelData.isHost ? "房主" : "成员")
                                          + (modelData.clientId === roomManager.clientId ? " · 你" : "")
                                    font.pixelSize: 11
                                    color: Style.textSecondary
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
                            text: roomManager.roomId
                            font.pixelSize: 13
                            font.bold: true
                            color: Style.accent
                        }
                    }
                }
            }

            Card {
                width: parent.width
                height: parent.height - membersCard.height - 20
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

                    Row {
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
