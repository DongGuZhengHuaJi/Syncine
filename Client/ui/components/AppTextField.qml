import QtQuick
import QtQuick.Controls
import SyncineApp

Item {
    id: root
    implicitWidth: column.implicitWidth
    implicitHeight: column.implicitHeight

    property alias label: labelText.text
    property alias text: field.text
    property alias placeholderText: field.placeholderText
    property alias echoMode: field.echoMode
    property alias inputMethodHints: field.inputMethodHints
    property alias validator: field.validator
    property alias maximumLength: field.maximumLength
    property bool passwordToggle: false
    property string errorText: ""
    signal accepted()

    function clear() {
        field.clear()
    }

    Column {
        id: column
        width: parent.width
        spacing: 8

        Label {
            id: labelText
            visible: text !== ""
            font.pixelSize: 13
            font.weight: Font.Medium
            color: Style.textSecondary
        }

        TextField {
            id: field
            width: parent.width
            height: Style.inputHeight
            leftPadding: 16
            rightPadding: root.passwordToggle ? 80 : 16
            font.pixelSize: 15
            color: Style.textPrimary
            placeholderTextColor: Style.textSecondary
            selectByMouse: true

            Keys.onReturnPressed: root.accepted()

            background: Rectangle {
                radius: Style.radiusSmall
                color: field.hovered ? Style.fieldHover : Style.fieldBg
                border.width: 1
                border.color: field.activeFocus ? Style.accent
                            : (root.errorText !== "" ? Style.danger : Style.border)
            }

            ToolButton {
                visible: root.passwordToggle
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 56
                height: 30

                contentItem: Text {
                    text: field.echoMode === TextInput.Normal ? "隐藏" : "显示"
                    font.pixelSize: 12
                    color: Style.accent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                background: Item {}

                onClicked: field.echoMode = field.echoMode === TextInput.Normal
                                           ? TextInput.Password : TextInput.Normal
            }
        }

        Label {
            visible: root.errorText !== ""
            text: root.errorText
            font.pixelSize: 12
            color: Style.danger
        }
    }
}
