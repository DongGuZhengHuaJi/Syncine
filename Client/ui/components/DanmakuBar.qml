import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SyncineApp

// 非全屏时贴在播放器下方的弹幕输入条。
//
// 它和右边评论区的输入框是同一个东西(同一份消息列表),
// 只是位置不同:非全屏在播放器下面,全屏时并进控制条。
Rectangle {
    id: root

    signal submitted(string text)

    implicitHeight: 50
    color: Style.card

    function submit() {
        const t = field.text.trim()
        if (t === "")
            return
        root.submitted(t)
        field.clear()
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Style.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 14
        spacing: 10

        TextField {
            id: field
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            leftPadding: 12
            rightPadding: 12
            font.pixelSize: 13
            color: Style.textPrimary
            placeholderText: "发个友善的弹幕见证当下"
            placeholderTextColor: Style.textFaint
            selectByMouse: true

            Keys.onReturnPressed: root.submit()

            background: Rectangle {
                radius: Style.radiusSmall
                color: field.hovered ? Style.fieldHover : Style.card
                border.width: 1
                border.color: field.activeFocus ? Style.accent : Style.border
            }
        }

        Button {
            id: sendBtn
            Layout.preferredWidth: 64
            Layout.preferredHeight: 32
            hoverEnabled: true

            contentItem: Text {
                text: "发送"
                font.pixelSize: 13
                color: Style.textOnAccent
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                radius: Style.radiusSmall
                color: sendBtn.pressed ? Style.accentPressed
                     : (sendBtn.hovered ? Style.accentHover : Style.accent)
            }

            onClicked: root.submit()
        }
    }
}
