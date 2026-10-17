import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SyncineApp

// 与某位成员的文件传输窗口。
//
// 打开方式：成员列表每行的邮件按钮，或收到对方的发送请求时由 RoomPage 打开。
// 窗口只是视图：传输状态保存在 FileTransferManager 里，关掉窗口不会中断任务，
// 重新打开仍能看到进行中的进度与历史。
//
// 坐标基准是声明它的 Item（RoomPage 根元素），与 AddVideoDialog 同理。
Popup {
    id: root

    property string peerId: ""
    property string peerName: ""
    // 对方的数据通道是否已打开。文件只能走已建立的连接
    property bool channelOpen: false
    // 由 fileTransferManager.transfersFor(peerId) 提供
    property var transferModel: null
    property string errorText: ""
    // 打开文件选择框时对应的对端。选择期间若切到别人，文件仍发给原对端
    property string pendingPeerId: ""

    readonly property int transferCount: transferModel ? transferModel.count : 0
    readonly property int activeCount: transferModel ? transferModel.activeCount : 0

    // 打开某位成员的窗口。peerName 为空时用 clientId 前几位兜底
    function openFor(peer, name) {
        peerId = peer
        peerName = name !== "" ? name : peer.substring(0, 8)
        transferModel = fileTransferManager.transfersFor(peer)
        channelOpen = fileTransferManager.isConnected(peer)
        errorText = ""
        open()
    }

    modal: false
    focus: true
    // 点外面也关：传输在后台继续，不存在"误关导致失败"的问题
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    width: 480
    height: 560
    padding: 0
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)

    onClosed: {
        sendDialog.close()
        pendingPeerId = ""
    }

    background: Rectangle {
        radius: Style.radius
        color: Style.card
        border.width: 1
        border.color: Style.border
    }

    Connections {
        target: fileTransferManager

        function onConnectionChanged(peer) {
            if (peer === root.peerId)
                root.channelOpen = fileTransferManager.isConnected(peer)
        }

        function onErrorOccurred(peer, message) {
            if (peer === root.peerId)
                root.errorText = message
        }
    }

    FileDialog {
        id: sendDialog
        title: "选择要发送的文件"
        fileMode: FileDialog.OpenFile
        nameFilters: ["所有文件 (*)"]
        onAccepted: fileTransferManager.sendFile(root.pendingPeerId !== ""
                                                 ? root.pendingPeerId : root.peerId,
                                                 selectedFile)
    }

    contentItem: ColumnLayout {
        width: parent.width
        height: parent.height
        spacing: 0

        // ── 头部 ──────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 64
            color: "transparent"

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Style.border
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 14
                spacing: 10

                Rectangle {
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    radius: 17
                    color: Style.accentSoft

                    Text {
                        anchors.centerIn: parent
                        text: root.peerName.length > 0 ? root.peerName.charAt(0) : "?"
                        font.pixelSize: 13
                        font.bold: true
                        color: Style.accent
                    }
                }

                ColumnLayout {
                    spacing: 2

                    Text {
                        text: "与 " + root.peerName + " 的文件传输"
                        font.pixelSize: 15
                        font.bold: true
                        color: Style.textPrimary
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Text {
                        text: root.channelOpen ? "点对点直传 · 文件不经过服务器"
                                              : "正在建立数据连接…"
                        font.pixelSize: 11
                        color: root.channelOpen ? Style.textFaint : Style.brand
                    }
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: "✕"
                    font.pixelSize: 15
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
        }

        // ── 工具栏 ────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 14
            Layout.topMargin: 12
            Layout.bottomMargin: 8
            spacing: 10

            Button {
                id: sendButton
                Layout.preferredWidth: 96
                Layout.preferredHeight: 34
                enabled: root.channelOpen
                opacity: enabled ? 1.0 : 0.5

                contentItem: Item {
                    Row {
                        anchors.centerIn: parent
                        spacing: 6

                        AppIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14
                            height: 14
                            name: "plus"
                            color: Style.textOnAccent
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "发送文件"
                            font.pixelSize: 13
                            color: Style.textOnAccent
                        }
                    }
                }

                background: Rectangle {
                    radius: Style.radiusSmall
                    color: !sendButton.enabled ? Style.fieldHover
                         : (sendButton.pressed ? Style.accentPressed
                         : (sendButton.hovered ? Style.accentHover : Style.accent))
                }

                onClicked: {
                    root.pendingPeerId = root.peerId
                    sendDialog.open()
                }
            }

            Text {
                text: root.activeCount > 0
                      ? root.activeCount + " 个传输进行中"
                      : (root.channelOpen ? "可发送文件" : "连接就绪后才能发送")
                font.pixelSize: 11
                color: Style.textFaint
            }

            Item { Layout.fillWidth: true }

            Text {
                visible: root.transferCount > 0
                text: "共 " + root.transferCount + " 条记录"
                font.pixelSize: 11
                color: Style.textFaint
            }
        }

        // 通道级错误（例如未连接就点了发送）
        Text {
            visible: root.errorText !== ""
            Layout.fillWidth: true
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.bottomMargin: 6
            text: root.errorText
            wrapMode: Text.Wrap
            font.pixelSize: 11
            color: Style.danger
        }

        // ── 传输记录 ──────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: transferList
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.topMargin: 4
                anchors.bottomMargin: 6
                clip: true
                spacing: 6
                model: root.transferModel

                delegate: Item {
                    width: transferList.width
                    height: card.height + 12

                    Rectangle {
                        id: card
                        width: parent.width
                        height: body.height + 20
                        radius: Style.radiusSmall
                        color: active ? Style.accentSoft
                             : (transferState === "failed" ? "#FFF3F3" : Style.fieldBg)

                        ColumnLayout {
                            id: body
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 10
                            // 显式跟随隐式高度：卡片高度由它反推
                            height: implicitHeight
                            spacing: 4

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8

                                AppIcon {
                                    Layout.preferredWidth: 17
                                    Layout.preferredHeight: 17
                                    Layout.alignment: Qt.AlignTop
                                    name: "file"
                                    color: active ? Style.accent : Style.textSecondary
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1

                                    Text {
                                        Layout.fillWidth: true
                                        text: fileName
                                        elide: Text.ElideMiddle
                                        font.pixelSize: 13
                                        color: Style.textPrimary
                                    }

                                    Text {
                                        text: directionText + " · " + sizeText
                                        font.pixelSize: 10
                                        color: Style.textFaint
                                    }

                                    Text {
                                        text: stateText
                                              + (timeText !== "" ? " · " + timeText : "")
                                        font.pixelSize: 10
                                        color: transferState === "failed" ? Style.danger
                                             : (transferState === "completed" ? Style.success
                                             : (transferState === "waiting" ? Style.brand
                                                                            : Style.textSecondary))
                                    }
                                }

                                TextAction {
                                    visible: canAccept
                                    text: "接收"
                                    emphasized: true
                                    onClicked: fileTransferManager.acceptTransfer(root.peerId, transferId)
                                }

                                TextAction {
                                    visible: canReject
                                    text: "拒绝"
                                    onClicked: fileTransferManager.rejectTransfer(root.peerId, transferId)
                                }

                                TextAction {
                                    visible: canCancel
                                    text: "取消"
                                    onClicked: fileTransferManager.cancelTransfer(root.peerId, transferId)
                                }

                                TextAction {
                                    visible: transferState === "completed"
                                             && direction === "receive"
                                             && targetPath !== ""
                                    text: "打开文件夹"
                                    emphasized: true
                                    onClicked: fileTransferManager.revealTransfer(root.peerId, transferId)
                                }
                            }

                            // 进度：条 + 百分比 + 速度
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 2
                                visible: transferState === "transferring"
                                spacing: 8

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 4
                                    radius: 2
                                    color: "#FFFFFF"

                                    Rectangle {
                                        width: parent.width * progress / 100
                                        height: parent.height
                                        radius: parent.radius
                                        color: Style.accent
                                    }
                                }

                                Text {
                                    text: progress + "%"
                                    font.pixelSize: 11
                                    color: Style.accent
                                }

                                Text {
                                    visible: speedText !== ""
                                    text: speedText
                                    font.pixelSize: 11
                                    color: Style.textFaint
                                }
                            }

                            Text {
                                visible: transferState === "transferring"
                                text: transferredText + " / " + sizeText
                                font.pixelSize: 10
                                color: Style.textFaint
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: transferError !== ""
                                text: transferError
                                wrapMode: Text.Wrap
                                font.pixelSize: 11
                                color: Style.danger
                            }

                            // 校验结果单独标一行，避免与普通状态混在一起
                            Text {
                                visible: verified
                                text: "SHA-256 校验通过"
                                font.pixelSize: 10
                                color: Style.success
                            }
                        }
                    }
                }
            }

            // 空态
            Column {
                anchors.centerIn: parent
                width: 240
                spacing: 10
                visible: root.transferCount === 0

                AppIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 36
                    height: 36
                    name: "file"
                    color: Style.border
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "还没有传输记录"
                    font.pixelSize: 13
                    color: Style.textFaint
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: root.channelOpen ? "点「发送文件」选择要传的文件"
                                           : "等数据连接建立后即可发送"
                    font.pixelSize: 11
                    color: Style.textFaint
                }
            }
        }

        // ── 底部提示 ──────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: "transparent"

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: 1
                color: Style.border
            }

            Text {
                anchors.centerIn: parent
                text: "关闭窗口不会中断正在进行的传输"
                font.pixelSize: 11
                color: Style.textFaint
            }
        }
    }

    // 行内文字按钮（只有这个文件用到）
    component TextAction: Text {
        id: action

        property bool emphasized: false
        signal clicked()

        font.pixelSize: 12
        color: emphasized ? Style.accent : Style.textSecondary

        MouseArea {
            anchors.fill: parent
            anchors.margins: -5
            cursorShape: Qt.PointingHandCursor
            onClicked: action.clicked()
        }
    }
}
