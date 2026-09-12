import QtQuick
import QtQuick.Controls
import SyncineApp

Button {
    id: modeBtn

    property bool selected: false

    height: 30
    opacity: enabled ? 1.0 : 0.5

    contentItem: Label {
        text: modeBtn.text
        font.pixelSize: 13
        font.bold: modeBtn.selected
        color: modeBtn.selected ? "#FFFFFF" : Style.textSecondary
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: 8
        color: modeBtn.selected ? Style.accent
              : (modeBtn.hovered ? Style.fieldHover : "transparent")
    }
}
