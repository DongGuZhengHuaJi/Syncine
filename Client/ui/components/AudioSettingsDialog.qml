import QtQuick
import QtQuick.Controls
import SyncineApp

// 房间里的音频设置(播放器控制条上的齿轮打开)。
//
// 为什么做成弹窗、入口还放在控制条上:右侧功能框只在"普通"形态下显示,
// 宽屏/全屏时看不见 —— 而"耳机插上了声音却还在外放"这种事,恰恰最可能在全屏
// 看片时才发现,得让人在全屏也够得着。
//
// 坐标基准是**声明它的那个 Item**。所以它必须声明在 RoomPage 根上
// (控制条上的齿轮只负责发信号),否则会以控制条为原点偏出去 ——
// 同一个坑在 AddVideoDialog 里踩过。
Popup {
    id: root

    modal: true
    focus: true
    // 点外面就关会让人以为"是不是选错了";只留 Esc(同 AddVideoDialog)
    closePolicy: Popup.CloseOnEscape

    width: 420
    // 内边距写在 Popup 上,不要写在 contentItem 里(理由见 AddVideoDialog:
    // Control 会把 contentItem 的尺寸设成「窗口尺寸 - 内边距」,子项再
    // width: parent.width 才正好贴合白卡片)
    padding: 20

    // 显式居中(默认 Popup 不保证居中)
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)

    // 每次打开都重新枚举:设备可能是进房之后才插上/拔掉的
    onOpened: signalingChannel.refreshAudioDevices()

    background: Rectangle {
        radius: Style.radius
        color: Style.card
        border.width: 1
        border.color: Style.border
    }

    contentItem: Column {
        id: content
        spacing: 14

        // ── 头部:标题 + 关闭 ──
        Item {
            width: parent.width
            height: 22

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "声音设置"
                font.pixelSize: 16
                font.bold: true
                color: Style.textPrimary
            }

            Text {
                id: closeBtn
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "✕"
                font.pixelSize: 14
                color: closeHover.containsMouse ? Style.textPrimary : Style.textFaint

                MouseArea {
                    id: closeHover
                    anchors.fill: parent
                    anchors.margins: -6
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.close()
                }
            }
        }

        AudioDeviceSettings {
            width: parent.width
        }
    }
}
