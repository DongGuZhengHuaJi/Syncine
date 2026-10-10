import QtQuick
import QtQuick.Controls
import SyncineApp

// 主按钮:实心主色。禁用时用浅蓝线色,而不是变灰 —— 页面底色是白的,
// 灰按钮在浅色背景上几乎看不见。
Button {
    id: root

    height: Style.inputHeight
    font.pixelSize: 15
    font.bold: true
    hoverEnabled: true

    contentItem: Text {
        text: root.text
        font: root.font
        color: Style.textOnAccent
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        opacity: root.enabled ? 1.0 : 0.75
    }

    background: Rectangle {
        radius: Style.radiusSmall
        color: !root.enabled ? Style.accentLine
             : root.pressed ? Style.accentPressed
             : (root.hovered ? Style.accentHover : Style.accent)
    }
}
