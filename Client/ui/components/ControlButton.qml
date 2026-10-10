import QtQuick
import QtQuick.Controls
import SyncineApp

// 播放器控制条上的按钮(深色底那一套)。
// 图标按钮给 icon,纯文字按钮(如 -10 / +10)给 text。
AbstractButton {
    id: root

    property string iconName: ""
    // 激活态:麦克风开着、弹幕开着……用主色描出来
    property bool active: false
    property string tip: ""

    implicitHeight: 30
    implicitWidth: iconName !== "" ? 30 : Math.max(30, labelText.implicitWidth + 14)
    hoverEnabled: true

    contentItem: Item {
        AppIcon {
            anchors.centerIn: parent
            visible: root.iconName !== ""
            width: 18
            height: 18
            name: root.iconName
            color: root.active ? Style.accent
                               : (root.enabled ? Style.playerIcon : Style.playerIconDim)
        }

        Text {
            id: labelText
            anchors.centerIn: parent
            visible: root.iconName === ""
            text: root.text
            font.pixelSize: 12
            font.bold: true
            color: root.active ? Style.accent
                               : (root.enabled ? Style.playerIcon : Style.playerIconDim)
        }
    }

    background: Rectangle {
        radius: Style.radiusSmall
        color: root.hovered && root.enabled ? "#29FFFFFF" : "transparent"
    }

    ToolTip.visible: hovered && tip !== ""
    ToolTip.text: tip
    ToolTip.delay: 500
}
