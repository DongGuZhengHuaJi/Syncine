import QtQuick
import QtQuick.Controls
import SyncineApp

Item {
    // 设置入口:挨着右上角的状态胶囊。全局设置都在这页里改
    // (房间内只放麦克风/听筒/cookie 那几项)。
    Button {
        id: settingsBtn
        anchors.top: parent.top
        anchors.right: statusPill.left
        anchors.topMargin: 24
        anchors.rightMargin: 8
        width: 32
        height: 32
        hoverEnabled: true

        contentItem: AppIcon {
            anchors.centerIn: parent
            width: 18
            height: 18
            name: "gear"
            color: settingsBtn.hovered ? Style.accent : Style.textSecondary
        }

        background: Rectangle {
            radius: Style.radius
            color: settingsBtn.hovered ? Style.hover : Style.card
            border.width: 1
            border.color: Style.border
        }

        ToolTip.visible: hovered
        ToolTip.text: "设置"
        ToolTip.delay: 500

        onClicked: stackView.push("SettingsPage.qml")
    }

    Rectangle {
        id: statusPill
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 24
        width: statusRow.implicitWidth + 28
        height: 32
        radius: Style.radius
        color: Style.card
        border.width: 1
        border.color: Style.border

        Row {
            id: statusRow
            anchors.centerIn: parent
            spacing: 8

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 8
                height: 8
                radius: 4
                color: networkManager.isConnected ? Style.success
                     : networkManager.connecting ? Style.accent
                     : networkManager.retrying ? Style.danger
                     : Style.textSecondary
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                font.pixelSize: 12
                color: Style.textSecondary
                text: {
                    if (networkManager.isConnected)
                        return networkManager.usingFallback ? "已连接(备用地址)"
                                                            : "服务器已连接"
                    if (networkManager.connecting)
                        return "连接中…"
                    if (networkManager.retrying)
                        return "未连接 · 重试中"
                    return "服务器未连接"
                }
            }
        }
    }

    Column {
        anchors.centerIn: parent
        width: 560
        spacing: 0

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            width: 88
            height: 88
            radius: 8
            color: Style.accent

            Canvas {
                anchors.centerIn: parent
                width: 34
                height: 40
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.fillStyle = "#FFFFFF"
                    ctx.beginPath()
                    ctx.moveTo(2, 0)
                    ctx.lineTo(34, 20)
                    ctx.lineTo(2, 40)
                    ctx.closePath()
                    ctx.fill()
                }
            }
        }

        Item { width: 1; height: 28 }

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Syncine"
            font.pixelSize: 52
            font.bold: true
            color: Style.textPrimary
        }

        Item { width: 1; height: 12 }

        // Label {
        //     anchors.horizontalCenter: parent.horizontalCenter
        //     text: "与好友一起,同步每一帧"
        //     font.pixelSize: 16
        //     color: Style.textSecondary
        // }

        Item { width: 1; height: 52 }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 24

            Rectangle {
                id: createCard
                width: 268
                height: 164
                radius: 6
                // 悬停只改颜色,不做缩放 —— 缩放在桌面端反而显得飘
                color: createArea.containsMouse ? Style.accentHover : Style.accent

                Column {
                    anchors.centerIn: parent
                    spacing: 14

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 44
                        height: 44
                        radius: 6
                        color: "#33FFFFFF"

                        Rectangle {
                            anchors.centerIn: parent
                            width: 18
                            height: 2
                            radius: 1
                            color: "#FFFFFF"
                        }

                        Rectangle {
                            anchors.centerIn: parent
                            width: 2
                            height: 18
                            radius: 1
                            color: "#FFFFFF"
                        }
                    }

                    Column {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 6

                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "创建房间"
                            font.pixelSize: 19
                            font.bold: true
                            color: "#FFFFFF"
                        }

                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "新建房间,邀请好友加入"
                            font.pixelSize: 13
                            color: "#CCFFFFFF"
                        }
                    }
                }

                MouseArea {
                    id: createArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: stackView.push("CreateRoomPage.qml")
                }
            }

            Rectangle {
                id: joinCard
                width: 268
                height: 164
                radius: 6
                color: Style.card
                border.width: 1
                border.color: joinArea.containsMouse ? Style.accent : Style.border

                Column {
                    anchors.centerIn: parent
                    spacing: 14

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 44
                        height: 44
                        radius: 6
                        color: Style.accentSoft

                        Canvas {
                            anchors.centerIn: parent
                            width: 26
                            height: 22
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.strokeStyle = Style.accent
                                ctx.lineWidth = 3
                                ctx.lineCap = "round"
                                ctx.beginPath()
                                ctx.moveTo(2, 11)
                                ctx.lineTo(20, 11)
                                ctx.moveTo(13, 4)
                                ctx.lineTo(20, 11)
                                ctx.lineTo(13, 18)
                                ctx.stroke()
                            }
                        }
                    }

                    Column {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 6

                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "加入房间"
                            font.pixelSize: 19
                            font.bold: true
                            color: Style.textPrimary
                        }

                        Label {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: "输入房间号,进入好友的房间"
                            font.pixelSize: 13
                            color: Style.textSecondary
                        }
                    }
                }

                MouseArea {
                    id: joinArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: stackView.push("JoinRoomPage.qml")
                }
            }
        }

        Item { width: 1; height: 44 }

        // Label {
        //     anchors.horizontalCenter: parent.horizontalCenter
        //     text: "免费 · 无需注册 · 即开即用"
        //     font.pixelSize: 12
        //     color: Style.textSecondary
        //     opacity: 0.7
        // }
    }
}
