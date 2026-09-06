import QtQuick
import QtQuick.Controls
import SyncineApp

Item {
    property string roomName: ""
    property string nickname: ""

    ListModel {
        id: messageModel
        ListElement { who: "系统"; text: "欢迎来到房间,把房间号发给好友一起看片" }
    }

    function sendMessage() {
        var t = chatInput.text.trim()
        if (t === "")
            return
        messageModel.append({ "who": nickname, "text": t })
        chatInput.clear()
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
                onClicked: stackView.pop()
            }

            Label {
                height: 40
                verticalAlignment: Text.AlignVCenter
                text: roomName !== "" ? roomName : "房间"
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
                        text: "1 人在线"
                        font.pixelSize: 11
                        color: Style.accent
                    }
                }
            }
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
                source: "file:///home/donggu/下载/QQ202695-222944.mp4"
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
                padding: 20

                Column {
                    width: parent.width
                    spacing: 14

                    Row {
                        width: parent.width
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
                                    text: "1 人"
                                    font.pixelSize: 11
                                    color: Style.textSecondary
                                }
                            }
                        }
                    }

                    Row {
                        width: parent.width
                        spacing: 12

                        Rectangle {
                            width: 38
                            height: 38
                            radius: 19
                            color: Style.accent

                            Label {
                                anchors.centerIn: parent
                                text: nickname.length > 0 ? nickname.charAt(0) : "?"
                                font.pixelSize: 15
                                font.bold: true
                                color: "#FFFFFF"
                            }
                        }

                        Column {
                            spacing: 4

                            Label {
                                text: nickname !== "" ? nickname : "我"
                                font.pixelSize: 14
                                color: Style.textPrimary
                            }

                            Label {
                                text: "房主 · 你"
                                font.pixelSize: 11
                                color: Style.textSecondary
                            }
                        }
                    }

                    Label {
                        text: "把房间号发给好友,一起看片"
                        font.pixelSize: 12
                        color: Style.textSecondary
                        opacity: 0.8
                    }
                }
            }

            Card {
                width: parent.width
                height: parent.height - membersCard.height - 20
                padding: 20

                Column {
                    width: parent.width
                    height: parent.height
                    spacing: 12

                    Label {
                        text: "聊天"
                        font.pixelSize: 15
                        font.bold: true
                        color: Style.textPrimary
                    }

                    ListView {
                        id: chatList
                        width: parent.width
                        height: parent.height - 21 - 24 - Style.inputHeight
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
                        width: parent.width
                        spacing: 10

                        AppTextField {
                            id: chatInput
                            width: parent.width - 78
                            placeholderText: "说点什么…"
                            onAccepted: sendMessage()
                        }

                        PrimaryButton {
                            width: 68
                            height: Style.inputHeight
                            text: "发送"
                            onClicked: sendMessage()
                        }
                    }
                }
            }
        }
    }
}
