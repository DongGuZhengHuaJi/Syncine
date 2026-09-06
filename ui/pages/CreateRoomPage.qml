import QtQuick
import QtQuick.Controls
import SyncineApp

Item {
    property string nameError: ""
    property string nickError: ""

    function tryCreate() {
        nameError = roomNameField.text.trim() === "" ? "请填写房间名称" : ""
        nickError = nickField.text.trim() === "" ? "请填写昵称" : ""
        if (nameError === "" && nickError === "") {
            stackView.push("RoomPage.qml", {
                "roomName": roomNameField.text.trim(),
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
            text: "创建房间"
            font.pixelSize: 30
            font.bold: true
            color: Style.textPrimary
        }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "创建一个新房间,邀请好友一起观看"
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
                    id: roomNameField
                    width: parent.width
                    label: "房间名称"
                    placeholderText: "给你的房间起个名字"
                    errorText: nameError
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
                    placeholderText: "留空表示不设密码"
                    echoMode: TextInput.Password
                    passwordToggle: true
                }

                Item { width: 1; height: 2 }

                PrimaryButton {
                    width: parent.width
                    text: "创建房间"
                    onClicked: tryCreate()
                }
            }
        }
    }
}
