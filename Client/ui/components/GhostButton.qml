import QtQuick
import QtQuick.Controls
import SyncineApp

Button {
    id: root

    // 默认这套配色是给浅色页面的。放在深色底上（比如播放器画面上的浮层）
    // 要覆盖这两个 —— 深灰文字配近黑底等于看不见
    property color textColor: Style.textSecondary
    property color borderColor: Style.border

    height: 40
    font.pixelSize: 14
    leftPadding: 18
    rightPadding: 18

    contentItem: Text {
        text: root.text
        font: root.font
        color: root.hovered ? Style.accent : root.textColor
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: Style.radiusSmall
        color: root.hovered ? Style.fieldHover : "transparent"
        border.width: 1
        border.color: root.hovered ? Style.accent : root.borderColor
    }
}
