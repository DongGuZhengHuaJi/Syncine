import QtQuick
import QtQuick.Controls
import SyncineApp

// 一个音量按钮 + 鼠标悬停时向右展开的滑条。
//
// 电影音量和语音音量是**两个独立实例**:各自一个图标、各自一条滑条,
// 因为它们背后是两条独立的音轨(房主的电影声 / 大家的语音)。
Item {
    id: root

    property string icon: "speaker"
    property string tip: ""
    property real value: 0

    signal moved(real v)
    signal interacted()

    implicitHeight: 30
    implicitWidth: button.implicitWidth + slot.width

    Rectangle {
        anchors.fill: parent
        radius: Style.radiusSmall
        color: hoverArea.containsMouse ? "#29FFFFFF" : "transparent"
    }

    Row {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter

        ControlButton {
            id: button
            iconName: root.icon
            tip: root.tip
        }

        Item {
            id: slot
            height: parent.height
            width: hoverArea.containsMouse ? 76 : 0
            clip: true
            Behavior on width {
                NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
            }

            Slider {
                id: slider
                anchors.left: parent.left
                anchors.leftMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                width: 66
                height: 20

                from: 0
                to: 1
                // 只读绑定:真身在 SignalingChannel,onMoved 写回去让它通知回来。
                // 直接给 value 赋值会打断这条绑定,滑条就再也不跟着状态走了。
                value: root.value

                onMoved: {
                    root.moved(value)
                    root.interacted()
                }
                onPressedChanged: root.interacted()

                background: Rectangle {
                    x: slider.leftPadding
                    y: slider.topPadding + slider.availableHeight / 2 - height / 2
                    implicitWidth: 66
                    implicitHeight: 3
                    width: slider.availableWidth
                    height: implicitHeight
                    color: Style.playerTrack

                    Rectangle {
                        width: slider.visualPosition * parent.width
                        height: parent.height
                        color: Style.accent
                    }
                }

                handle: Rectangle {
                    x: slider.leftPadding + slider.visualPosition
                       * (slider.availableWidth - width)
                    y: slider.topPadding + slider.availableHeight / 2 - height / 2
                    implicitWidth: 10
                    implicitHeight: 10
                    radius: 5
                    color: "#FFFFFF"
                }
            }
        }
    }

    // 悬停检测放在最上层、但不接收任何按键 ——
    // 这样整组都能感知悬停,下面的滑条照样能拖。
    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
    }
}
