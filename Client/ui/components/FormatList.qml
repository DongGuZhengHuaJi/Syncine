import QtQuick
import SyncineApp

// 详情视图中的一组流（视频 / 音频 / 合并格式），单选。
//
// 与 OptionList 的区别：
//   · 每行有两段文字（标题 + 右侧小字信息）
//   · 标出 HDR 的流——Qt 播放器播放 HDR 需离线转 SDR（见 HdrConverter），
//     用户需能看出所挑选的是哪一条
//   · 具有「手选」语义：点选后即为 pickedId，父级据此拼接 spec
//
// 三处约束（与 OptionList 相同）：
//   · **使用 Repeater + Column，不要用 ListView**（无隐式高度，放入 Popup 会塌陷）
//   · options 是 QVariantList，delegate 中**只有 modelData**，没有角色名
//   · 高度由行数自然撑开，是否需要滚动由调用方决定
Column {
    id: root

    // [{ id, title, detail, hdr }, ...]
    property var options: []
    // 选中的那条的 id(空 = 没选)
    property string pickedId: ""

    signal picked(string id)

    width: parent ? parent.width : 0
    spacing: 2

    Repeater {
        model: root.options

        delegate: Rectangle {
            required property var modelData

            readonly property bool selected: root.pickedId === modelData.id

            width: root.width
            height: 36
            radius: Style.radiusSmall
            color: selected ? Style.accentSoft : "transparent"
            border.width: 1
            border.color: selected ? Style.accent : Style.border

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, parent.width * 0.62)
                    text: modelData.title
                    elide: Text.ElideRight
                    font.pixelSize: 12
                    color: selected ? Style.accent : Style.textPrimary
                }

                // HDR 标：提醒播放器要转一道
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: modelData.hdr === true
                    leftPadding: 6
                    text: "HDR"
                    font.pixelSize: 10
                    color: Style.danger
                }

                Item {
                    width: parent.width - x - rightText.width
                    height: 1
                }

                Text {
                    id: rightText
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.detail
                    elide: Text.ElideRight
                    font.pixelSize: 11
                    color: Style.textFaint
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.picked(modelData.id)
            }
        }
    }
}
