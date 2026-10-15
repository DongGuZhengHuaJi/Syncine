import QtQuick
import SyncineApp

// 可点击的单选行列表（设置页的「日志级别」「设备选择」「默认画质」均使用它）。
// 自行实现是因为项目未引入 ComboBox/Switch，且下拉框在浅色卡片中观感不佳。
//
// 用法：
//     OptionList {
//         options: [{ label: "最高画质", value: 0 }, { label: "720P", value: 720 }]
//         selectedValue: appSettings.defaultQuality
//         onPicked: (value) => appSettings.setDefaultQuality(value)
//     }
//
// 两处约束：**使用 Repeater + Column，不要用 ListView**（ListView 是 Flickable，
// 无隐式高度，放入 Popup 等自适应容器会塌陷为 0）；options 是 JS 数组，
// delegate 中**只有 modelData**、没有角色名（`model.label` 取不到值）。
Column {
    id: root

    // [{ label: "...", value: ... }, ...]
    property var options: []
    // 当前选中项的 value(和 options 里的 value 用 === 比)
    property var selectedValue

    signal picked(var value)

    width: parent ? parent.width : 0
    spacing: 2

    Repeater {
        model: root.options

        delegate: Rectangle {
            required property var modelData

            width: root.width
            height: 32
            radius: Style.radiusSmall
            border.width: 1
            color: root.selectedValue === modelData.value ? Style.accentSoft : "transparent"
            border.color: root.selectedValue === modelData.value ? Style.accent : Style.border

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: modelData.label
                font.pixelSize: 12
                color: root.selectedValue === modelData.value ? Style.accent : Style.textSecondary
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.picked(modelData.value)
            }
        }
    }
}
