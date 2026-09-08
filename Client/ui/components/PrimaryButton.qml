import QtQuick
import QtQuick.Controls
import SyncineApp

Button {
    id: root

    height: Style.inputHeight
    font.pixelSize: 15
    font.bold: true

    contentItem: Text {
        text: root.text
        font: root.font
        color: Style.textOnAccent
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        opacity: root.enabled ? 1.0 : 0.5
    }

    background: Rectangle {
        radius: Style.radiusSmall
        color: root.down ? Style.accentPressed : Style.accent
    }
}
