import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SyncineApp

// 屏幕/窗口共享选择框（Phase 1）。
//
// 只负责收集选择并调用 ScreenShareController；不保存共享状态，
// 一切以 screenShare.active / screens / windows 为准。
// 弹窗声明在 RoomPage 根上：Popup 的坐标以声明它的 Item 为基准。
Popup {
    id: root

    // 0 = 整个屏幕，1 = 窗口
    property int sourceType: 0
    property int selectedIndex: -1

    readonly property var options: sourceType === 0
                                   ? screenShare.screens
                                   : screenShare.windows
    readonly property bool canStart: screenShare.active || selectedIndex >= 0

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape

    width: 480
    padding: 20

    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)

    onOpened: {
        sourceType = 0
        selectedIndex = -1
        screenShare.refreshSources()
    }

    function startSelected() {
        if (screenShare.active) {
            screenShare.stop()
            root.close()
            return
        }
        if (selectedIndex < 0)
            return

        const ok = sourceType === 0
                   ? screenShare.startScreen(selectedIndex)
                   : screenShare.startWindow(selectedIndex)
        if (ok)
            root.close()
    }

    background: Rectangle {
        radius: Style.radius
        color: Style.card
        border.width: 1
        border.color: Style.border
    }

    contentItem: ColumnLayout {
        spacing: 12

        Text {
            text: "共享屏幕"
            font.pixelSize: 16
            font.bold: true
            color: Style.textPrimary
        }

        // 正在共享时只显示状态和停止按钮；选择源会要求先停止，
        // 避免在采集过程中切换 QMediaCaptureSession 的目标。
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10
            visible: !screenShare.active

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                radius: Style.radius
                color: Style.fieldBg

                Row {
                    anchors.centerIn: parent
                    spacing: 2

                    ModeButton {
                        width: 100
                        text: "整个屏幕"
                        selected: root.sourceType === 0
                        onClicked: {
                            root.sourceType = 0
                            root.selectedIndex = -1
                        }
                    }

                    ModeButton {
                        width: 100
                        text: "窗口"
                        selected: root.sourceType === 1
                        onClicked: {
                            root.sourceType = 1
                            root.selectedIndex = -1
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.options.length === 0
                text: root.sourceType === 0
                      ? "没有可用屏幕"
                      : "当前平台没有可枚举的窗口（Wayland 下常见）。可改用「整个屏幕」，由系统选择窗口。"
                wrapMode: Text.Wrap
                font.pixelSize: 12
                color: Style.textSecondary
            }

            ListView {
                id: optionList
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(196, Math.max(36, root.options.length * 34))
                visible: root.options.length > 0
                clip: true
                spacing: 2
                model: root.options

                ScrollBar.vertical: ScrollBar {}

                delegate: Rectangle {
                    required property var modelData

                    width: optionList.width
                    height: 32
                    radius: Style.radiusSmall
                    border.width: 1
                    color: root.selectedIndex === modelData.value
                           ? Style.accentSoft : "transparent"
                    border.color: root.selectedIndex === modelData.value
                                  ? Style.accent : Style.border

                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.label
                        elide: Text.ElideMiddle
                        font.pixelSize: 12
                        color: root.selectedIndex === modelData.value
                               ? Style.accent : Style.textSecondary
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.selectedIndex = modelData.value
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: screenShare.active
            text: "正在共享：" + screenShare.sourceLabel
            wrapMode: Text.Wrap
            font.pixelSize: 13
            color: Style.accent
        }

        // ---- 系统声音 ----
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 5

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Text {
                    text: "共享系统声音"
                    font.pixelSize: 13
                    color: screenShare.audioAvailable ? Style.textPrimary
                                                      : Style.textFaint
                }

                Text {
                    visible: screenShare.active && screenShare.audioActive
                    text: "正在采集"
                    font.pixelSize: 11
                    color: Style.accent
                }

                Item { Layout.fillWidth: true }

                Rectangle {
                    id: audioSwitch
                    width: 40
                    height: 22
                    radius: 11
                    opacity: screenShare.audioAvailable ? 1.0 : 0.45
                    color: screenShare.audioEnabled && screenShare.audioAvailable
                           ? Style.accent : "#D9DEE5"

                    Rectangle {
                        width: 16
                        height: 16
                        radius: 8
                        y: 3
                        x: screenShare.audioEnabled && screenShare.audioAvailable
                           ? parent.width - width - 3 : 3
                        color: "#FFFFFF"
                        Behavior on x {
                            NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: screenShare.audioAvailable
                        cursorShape: Qt.PointingHandCursor
                        onClicked: screenShare.audioEnabled = !screenShare.audioEnabled
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: !screenShare.audioAvailable
                text: "当前平台或构建不支持系统声音采集"
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: Style.textFaint
            }

            Text {
                Layout.fillWidth: true
                visible: screenShare.audioAvailable && screenShare.audioEnabled
                text: "系统声音包含其他应用的输出。本软件自身的声音会被排除：Windows 使用进程回环，Linux 使用虚拟 Sink 路由；系统不支持时会回退到整机采集，可能产生回声。"
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: Style.textSecondary
            }

            Text {
                Layout.fillWidth: true
                visible: screenShare.audioError !== ""
                text: screenShare.audioError
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: "#D03050"
            }
        }

        Text {
            Layout.fillWidth: true
            visible: screenShare.errorString !== ""
            text: screenShare.errorString
            wrapMode: Text.Wrap
            font.pixelSize: 12
            color: "#D03050"
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Item { Layout.fillWidth: true }

            GhostButton {
                Layout.preferredWidth: 84
                Layout.preferredHeight: 34
                text: "取消"
                onClicked: root.close()
            }

            PrimaryButton {
                Layout.preferredWidth: 104
                Layout.preferredHeight: 34
                font.pixelSize: 13
                text: screenShare.active ? "停止共享" : "开始共享"
                enabled: root.canStart
                onClicked: root.startSelected()
            }
        }
    }
}
