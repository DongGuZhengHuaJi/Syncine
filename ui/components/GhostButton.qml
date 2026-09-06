import QtQuick
import QtQuick.Controls
import SyncineApp

Button {
    id: root

    height: 40
    font.pixelSize: 14
    leftPadding: 18
    rightPadding: 18

    contentItem: Text {
        text: root.text
        font: root.font
        color: root.hovered ? Style.accent : Style.textSecondary
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: Style.radiusSmall
        color: root.hovered ? Style.fieldHover : "transparent"
        border.width: 1
        border.color: root.hovered ? Style.accent : Style.border
    }
}
