import QtQuick
import QtQuick.Controls
import SyncineApp

Dialog {
    id: root

    property string message: ""

    modal: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    width: 400
    padding: 0

    // 默认 Popup 不保证居中,显式放在父项中央
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)

    background: Rectangle {
        radius: Style.radius
        color: Style.card
        border.width: 1
        border.color: Style.border
    }

    contentItem: Column {
        width: root.width
        topPadding: 24
        bottomPadding: 24
        leftPadding: 24
        rightPadding: 24
        spacing: 16

        Row {
            spacing: 10

            Rectangle {
                width: 28
                height: 28
                radius: 14
                color: Style.accentSoft

                Label {
                    anchors.centerIn: parent
                    text: "!"
                    font.pixelSize: 15
                    font.bold: true
                    color: Style.accent
                }
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: "提示"
                font.pixelSize: 16
                font.bold: true
                color: Style.textPrimary
            }
        }

        Label {
            width: parent.width
            text: root.message
            wrapMode: Text.Wrap
            lineHeight: 1.4
            font.pixelSize: 14
            color: Style.textSecondary
        }

        PrimaryButton {
            width: parent.width/2
            text: "知道了"
            onClicked: root.close()
        }
    }
}
