import QtQuick
import QtQuick.Controls
import SyncineApp

Item {
    property string codeError: ""
    property string nickError: ""

    function tryJoin() {
        codeError = roomCodeField.text.trim().length !== 6 ? "房间号是 6 位数字" : ""
        nickError = nickField.text.trim() === "" ? "请填写昵称" : ""
        if (codeError === "" && nickError === "") {
            stackView.push("RoomPage.qml", {
                "roomName": "房间 " + roomCodeField.text.trim(),
                "nickname": nickField.text.trim()
            })
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
            text: "输入 6 位房间号,进入好友的房间"
            font.pixelSize: 14
            color: Style.textSecondary
        }

        Item { width: 1; height: 20 }

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
                    width: parent.width
                    label: "密码(可选)"
                    placeholderText: "房间有密码时填写"
                    echoMode: TextInput.Password
                    passwordToggle: true
                }

                Item { width: 1; height: 2 }

                PrimaryButton {
                    width: parent.width
                    text: "加入房间"
                    onClicked: tryJoin()
                }
            }
        }
    }
}
