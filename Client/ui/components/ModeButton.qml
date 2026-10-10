import QtQuick
import QtQuick.Controls
import SyncineApp

// 模式分段控件里的一格:选中是白底 + 主色文字(不是实心蓝块),
// 灰底槽由外层容器给。
Button {
    id: modeBtn

    property bool selected: false

    height: 26
    opacity: enabled ? 1.0 : 0.4

    contentItem: Text {
        text: modeBtn.text
        font.pixelSize: 13
        font.bold: modeBtn.selected
        color: modeBtn.selected
               ? Style.accent
               : (modeBtn.hovered && modeBtn.enabled ? Style.accent : Style.textSecondary)
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: Style.radiusTiny
        color: modeBtn.selected
               ? Style.card
               : (modeBtn.hovered && modeBtn.enabled ? Style.fieldHover : "transparent")
    }
}
