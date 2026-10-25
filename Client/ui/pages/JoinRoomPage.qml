import QtQuick
import QtQuick.Controls
import SyncineApp

Item {
    property string codeError: ""
    property string nickError: ""
    property string serverError: ""

    function tryJoin() {
        codeError = roomCodeField.text.trim().length !== 6 ? "房间号是 6 位数字" : ""
        nickError = nickField.text.trim() === "" ? "请填写昵称" : ""
        if (codeError === "" && nickError === "") {
            // 状态已在房间里但页面没跳过去：直接进房间
            if (roomSession.inRoom) {
                stackView.push("RoomPage.qml")
                return
            }
            serverError = ""
            sessionController.joinRoom(roomCodeField.text.trim(), nickField.text.trim(),
                                       passwordField.text)
        }
    }

    Connections {
        target: sessionController

        function onRoomJoined() {
            stackView.push("RoomPage.qml")
        }

        function onErrorOccurred(message) {
            serverError = message
        }
    }

    GhostButton {
        anchors.left: parent.left
        anchors.leftMargin: 28
        anchors.top: parent.top
        anchors.topMargin: 24
        text: "← 返回"
        onClicked: stackView.pop()
    }

    Column {
        anchors.centerIn: parent
        width: Style.pageWidth
        spacing: 12

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "加入房间"
            font.pixelSize: 30
            font.bold: true
            color: Style.textPrimary
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "输入 6 位房间号，进入好友的房间"
            font.pixelSize: 14
            color: Style.textSecondary
        }

        Item { width: 1; height: 20 }

        // 自动重连宽限期结束后，服务端可能还保留着上次的会话。
        // 直接 resume 可以恢复原 clientId（包括房主身份），避免出现幽灵成员。
        Card {
            width: parent.width
            visible: sessionController.canResumeLastSession

            Column {
                width: parent.width
                spacing: 10

                Label {
                    text: "上次会话未结束"
                    font.pixelSize: 16
                    font.bold: true
                    color: Style.textPrimary
                }

                Label {
                    width: parent.width
                    text: "房间 " + sessionController.resumeRoomId
                          + " 的记录可能还在服务端保留，可以恢复原来的身份（包括房主身份）。"
                    font.pixelSize: 12
                    color: Style.textSecondary
                    wrapMode: Text.Wrap
                }

                PrimaryButton {
                    width: parent.width
                    enabled: !sessionController.busy
                    text: sessionController.busy ? "恢复中…" : "恢复上次会话"
                    onClicked: {
                        serverError = ""
                        sessionController.resumeLastSession()
                    }
                }
            }
        }

        Card {
            width: parent.width

            Column {
                width: parent.width
                spacing: 18

                AppTextField {
                    id: roomCodeField
                    width: parent.width
                    label: "房间号"
                    placeholderText: "6 位数字房间号"
                    maximumLength: 6
                    inputMethodHints: Qt.ImhDigitsOnly
                    validator: RegularExpressionValidator { regularExpression: /[0-9]{0,6}/ }
                    errorText: codeError
                }

                AppTextField {
                    id: nickField
                    width: parent.width
                    label: "你的昵称"
                    placeholderText: "好友看到的名字"
                    errorText: nickError
                }

                AppTextField {
                    id: passwordField
                    width: parent.width
                    label: "密码(可选)"
                    placeholderText: "房间有密码时填写"
                    echoMode: TextInput.Password
                    passwordToggle: true
                }

                Item { width: 1; height: 2 }

                PrimaryButton {
                    width: parent.width
                    enabled: !sessionController.busy
                    text: sessionController.busy ? "连接中…" : "加入房间"
                    onClicked: tryJoin()
                }

                Label {
                    visible: serverError !== ""
                    width: parent.width
                    text: serverError
                    font.pixelSize: 13
                    color: Style.danger
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
