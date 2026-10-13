import QtQuick
import SyncineApp

// 详情视图里的一组流(视频 / 音频 / 合并格式),单选。
//
// 和 OptionList 的区别:
//   · 每行有两段文字(标题 + 右边的小字信息)
//   · 会标出 HDR 的流 —— Qt 播放器放 HDR 要离线转 SDR(见 HdrConverter),
//     用户得能看见自己挑的是哪条
//   · 有"手选"语义:点过了就是 pickedId,父级拿它去拼 spec
//
// 三条踩过的坑(和 OptionList 一样,别踩回去):
//   · **用 Repeater + Column,不要用 ListView**(没有隐式高度,放进 Popup 会塌)
//   · options 是 QVariantList,delegate 里**只有 modelData**,没有角色名
//   · 高度靠行数自己长出来,外层要不要滚动由调用方决定
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

                // HDR 标:提醒播放器要转一道
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
