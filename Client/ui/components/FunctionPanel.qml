import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SyncineApp

// 右侧功能框：评论 / 播放列表 / 成员 三个页签，顶部切换。
//
// 评论和弹幕是**同一份消息**(messages 由 RoomPage 传入):
// 这里发的会飘过画面，弹幕框发的会出现在这个列表里。
Rectangle {
    id: root

    property var messages: null
    property string currentTab: "comment"

    // 共享模式下只有房主能添加：别人的机器上没有那个文件，加进来谁也播不了
    readonly property bool canAddToPlaylist: roomSession.roomMode !== RoomSession.Share
                                             || roomSession.isHost

    signal commentSubmitted(string text)
    // 用户要往播放列表里加东西 → 由 RoomPage 弹文件选择框
    signal addRequested()
    // 网链模式加条目要粘链接、挑画质 → 由 RoomPage 弹那个两页的窗口
    signal urlAddRequested()
    // 要给某一条指定本机文件（本地模式下每人补自己的那一份）
    signal localFileRequested(string itemId)
    // 打开与某位成员的文件传输窗口
    signal fileTransferRequested(string peerId, string nickname)

    color: Style.card
    radius: Style.radius
    border.width: 1
    border.color: Style.border

    // 复制房间号要用剪贴板，而 QML 里能碰到剪贴板的只有 TextField/TextEdit
    TextField {
        id: clipboard
        visible: false
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── 页签 ────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            color: "transparent"

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Style.border
            }

            Row {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 20

                PanelTab {
                    text: "评论"
                    count: root.messages ? root.messages.count : 0
                    selected: root.currentTab === "comment"
                    onClicked: root.currentTab = "comment"
                }

                PanelTab {
                    text: "播放列表"
                    selected: root.currentTab === "playlist"
                    onClicked: root.currentTab = "playlist"
                }

                PanelTab {
                    text: "成员"
                    count: roomSession.members.count
                    selected: root.currentTab === "members"
                    onClicked: root.currentTab = "members"
                }
            }
        }

        // ── 评论 ────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.currentTab === "comment"

            ListView {
                id: commentList
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 6
                anchors.bottom: composer.top
                clip: true
                spacing: 2
                model: root.messages

                // 新消息进来就滚到底
                onCountChanged: positionViewAtEnd()

                delegate: Item {
                    width: commentList.width
                    // 系统消息固定 34;普通评论按正文实际高度撑开
                    height: model.who === "系统" ? 34 : body.height + 12

                    Rectangle {
                        visible: model.who === "系统"
                        anchors.centerIn: parent
                        width: sysText.implicitWidth + 20
                        height: 20
                        radius: Style.radiusSmall
                        color: Style.fieldBg

                        Text {
                            id: sysText
                            anchors.centerIn: parent
                            text: model.text
                            font.pixelSize: 11
                            color: Style.textFaint
                        }
                    }

                    // body 只按 top 定位，高度由正文撑 ——
                    // 如果改成 verticalCenter 就会和父项高度互相依赖，绑定成环。
                    Item {
                        id: body
                        visible: model.who !== "系统"
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: 6
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        height: commentText.y + commentText.height

                        Rectangle {
                            id: avatar
                            width: 30
                            height: 30
                            radius: 15
                            color: Style.accentSoft

                            Text {
                                anchors.centerIn: parent
                                text: model.who.length > 0 ? model.who.charAt(0) : "?"
                                font.pixelSize: 12
                                font.bold: true
                                color: Style.accent
                            }
                        }

                        Text {
                            id: nick
                            anchors.left: avatar.right
                            anchors.leftMargin: 9
                            anchors.top: parent.top
                            text: model.who
                            font.pixelSize: 12
                            font.bold: true
                            color: Style.textSecondary
                        }

                        Text {
                            id: commentText
                            anchors.left: nick.left
                            anchors.right: parent.right
                            anchors.top: nick.bottom
                            anchors.topMargin: 2
                            text: model.text
                            wrapMode: Text.Wrap
                            font.pixelSize: 13
                            color: Style.textPrimary
                            lineHeight: 1.35
                        }
                    }
                }
            }

            // ── 评论输入框 ──────────────────────────
            Rectangle {
                id: composer
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 54

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 1
                    color: Style.border
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 8

                    TextField {
                        id: commentField
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        leftPadding: 12
                        rightPadding: 12
                        font.pixelSize: 13
                        color: Style.textPrimary
                        placeholderText: "发条友善的评论吧"
                        placeholderTextColor: Style.textFaint
                        selectByMouse: true

                        Keys.onReturnPressed: submit()

                        function submit() {
                            const t = text.trim()
                            if (t === "")
                                return
                            root.commentSubmitted(t)
                            text = ""
                        }

                        background: Rectangle {
                            radius: Style.radiusSmall
                            color: commentField.hovered ? Style.fieldHover : Style.card
                            border.width: 1
                            border.color: commentField.activeFocus ? Style.accent : Style.border
                        }
                    }

                    Button {
                        id: commentSendBtn
                        Layout.preferredWidth: 56
                        Layout.preferredHeight: 34
                        hoverEnabled: true

                        contentItem: Text {
                            text: "发送"
                            font.pixelSize: 13
                            color: Style.textOnAccent
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }

                        background: Rectangle {
                            radius: Style.radiusSmall
                            color: commentSendBtn.pressed ? Style.accentPressed
                                 : (commentSendBtn.hovered ? Style.accentHover : Style.accent)
                        }

                        onClicked: commentField.submit()
                    }
                }
            }
        }

        // ── 播放列表 ────────────────────────────────
        //
        // 三种模式**各存一份**列表（服务端分的）,这里显示的是当前模式那一份：
        //   同步模式  房间共享的队列，每条要显示"大家都有没有"
        //   共享模式  只有房主有（条目就是他本机的文件）,没有"匹配"这回事
        //   网链模式  房间共享的队列，大家读同一个 URL
        Item {
            id: playlistTab
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.currentTab === "playlist"

            // 匹配仅在同步模式下有意义——共享模式中条目本就是房主自己的文件，
            // 网链模式各端读取同一 URL，均不存在「是否持有该条」的问题
            readonly property bool showsMatching: roomSession.roomMode === RoomSession.Sync

            // 网链模式：加条目要粘链接、挑画质，交给 RoomPage 的弹窗
            // (同步模式还是走"选本地文件"的对话框)
            readonly property bool isUrlMode: roomSession.roomMode === RoomSession.Url

            function subtitleFor(dur, url, isLive, label) {
                const parts = []
                // 直播没有时长：标"直播"
                parts.push(isLive ? "直播" : (dur > 0 ? Style.formatTime(dur) : "--:--"))
                parts.push(url !== "" ? "网链" : "本地文件")
                // 画质标签（"1080P"/"手选 720P"）。最高画质和直播不显示 ——
                // 免得每条都拖一句看不出信息的话（specLabel 里那两种就是它们）
                if (url !== "" && label !== "" && label !== "最高画质" && label !== "直播")
                    parts.push(label)
                return parts.join(" · ")
            }

            // 匹配状态是**服务端聚合**后下发的（它才知道所有成员的情况）:
            //   全员都有且内容一致 → 绿"已匹配"
            //   还有人没有         → 灰"未匹配"
            //   都有但内容不一样   → 红"不同步"
            function statusText(value) {
                if (value === "matched")
                    return "已匹配"
                if (value === "mismatch")
                    return "不同步"
                return "未匹配"
            }

            function statusColor(value) {
                if (value === "matched")
                    return Style.success
                if (value === "mismatch")
                    return Style.danger
                return Style.textFaint
            }

            // 头部：数量 + 权限说明 + 添加
            RowLayout {
                id: listHead
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                height: 34
                spacing: 8

                Text {
                    text: "共 " + roomSession.playlist.count + " 个"
                          // + (roomSession.isHost ? " · 房主可整理"
                          //                       : " · 房主管理")
                    font.pixelSize: 12
                    color: Style.textFaint
                }

                Item { Layout.fillWidth: true }

                Text {
                    text: "＋ 添加"
                    font.pixelSize: 12
                    color: root.canAddToPlaylist ? Style.accent : Style.textFaint
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -6
                        cursorShape: root.canAddToPlaylist ? Qt.PointingHandCursor
                                                           : Qt.ArrowCursor
                        onClicked: {
                            if (!root.canAddToPlaylist)
                                return
                            // 网链模式：弹"粘链接 + 选画质"的窗口；其它模式：选本机文件
                            if (playlistTab.isUrlMode)
                                root.urlAddRequested()
                            else
                                root.addRequested()
                        }
                    }
                }
            }

            ListView {
                id: playlistList
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: listHead.bottom
                anchors.bottom: parent.bottom
                clip: true
                spacing: 2
                model: roomSession.playlist

                delegate: Item {
                    width: playlistList.width

                    // 显示错误信息的属性
                    readonly property bool showError: playlistTab.isUrlMode && isCurrent
                                                      && playbackSync.downloadStage === "failed"

                    height: 54 + (showError ? errorColumn.implicitHeight + 14 : 0)

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 4
                        radius: Style.radiusSmall
                        color: isCurrent ? Style.accentSoft
                             : (rowHover.containsMouse ? Style.hover : "transparent")
                    }

                    MouseArea {
                        id: rowHover
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                    }

                    // 行内容固定在顶部 54px:失败时下面还要摊开错误信息，
                    // 不能让这一行跟着整块高度一起居中
                    RowLayout {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: 54
                        anchors.leftMargin: 10
                        anchors.rightMargin: 14
                        spacing: 8

                        Rectangle {
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 20
                            Layout.alignment: Qt.AlignVCenter
                            radius: Style.radiusTiny
                            color: isCurrent ? Style.accent : Style.fieldBg

                            Text {
                                anchors.centerIn: parent
                                text: index + 1
                                font.pixelSize: 11
                                color: isCurrent ? "#FFFFFF" : Style.textSecondary
                            }
                        }

                        Column {
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                width: parent.width
                                text: title
                                elide: Text.ElideRight
                                font.pixelSize: 13
                                color: isCurrent ? Style.accent : Style.textPrimary
                            }

                            Row {
                                spacing: 6

                                Text {
                                    text: playlistTab.subtitleFor(duration, url, live, specLabel)
                                    font.pixelSize: 11
                                    color: Style.textFaint
                                }

                                // 匹配状态：只有同步模式显示
                                Text {
                                    visible: playlistTab.showsMatching
                                    text: playlistTab.statusText(status)
                                    font.pixelSize: 11
                                    color: playlistTab.statusColor(status)
                                }
                            }
                        }

                        // 网链模式：当前条目的准备状态（解析中/下载中 42%/合并中/失败）。
                        // 用紧凑文案，完整的速度与剩余时间在画面中央的提示里
                        Text {
                            visible: playlistTab.isUrlMode && isCurrent
                                     && playbackSync.downloadShortText !== ""
                            Layout.alignment: Qt.AlignVCenter
                            text: playbackSync.downloadShortText
                            font.pixelSize: 11
                            color: playbackSync.downloadStage === "failed"
                                   ? Style.danger : Style.textFaint
                        }

                        // 网链模式：每个还没下好的条目都能单独重试。
                        // 当前条目失败/卡住时也能点；非当前条目会在后台下载并
                        // 写入缓存，不打断正在播放的条目。
                        Item {
                            readonly property bool retryable:
                                playlistTab.isUrlMode && !hasLocalFile
                                && (!live || isCurrent)
                            visible: retryable
                                     && playbackSync.retryingItemId !== itemId
                                     && (isCurrent || rowHover.containsMouse)
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 18
                            Layout.alignment: Qt.AlignVCenter

                            AppIcon {
                                anchors.centerIn: parent
                                width: 14
                                height: 14
                                name: "refresh"
                                color: retryHover.containsMouse
                                       ? Style.accentHover : Style.accent
                            }

                            MouseArea {
                                id: retryHover
                                anchors.fill: parent
                                anchors.margins: -4
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: playbackSync.retryItem(itemId)
                            }
                        }

                        // 后台重试中：给一行短提示（当前条目的进度在左边状态里）
                        Text {
                            visible: playlistTab.isUrlMode
                                     && playbackSync.retryingItemId === itemId
                            Layout.alignment: Qt.AlignVCenter
                            text: "重试中…"
                            font.pixelSize: 11
                            color: Style.textFaint
                        }

                        // 当前条目：一个状态标记，不给操作
                        Rectangle {
                            visible: isCurrent
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 18
                            Layout.alignment: Qt.AlignVCenter
                            radius: Style.radiusTiny
                            color: Style.accent

                            Text {
                                anchors.centerIn: parent
                                text: "播放中"
                                font.pixelSize: 10
                                color: "#FFFFFF"
                            }
                        }

                        // 非当前条目：房主可以切过去。
                        // 屏幕共享期间不能切：先停止共享，电影共享才会重新可用。
                        Text {
                            visible: !isCurrent && roomSession.isHost
                            Layout.alignment: Qt.AlignVCenter
                            text: roomSession.screenSharing ? "停止共享后可切换" : "切到此条"
                            font.pixelSize: 12
                            color: roomSession.screenSharing ? Style.textFaint : Style.accent
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                enabled: !roomSession.screenSharing
                                cursorShape: Qt.PointingHandCursor
                                onClicked: roomSession.switchTo(itemId)
                            }
                        }

                        // 指定/重选本机文件：同步模式下每个人给自己补那一份。
                        // 已经选过的条目悬停时会变成"重新选择" —— 换了版本、
                        // 文件挪了位置都要能改，不能一次定终身
                        Text {
                            visible: playlistTab.showsMatching
                                     && (!hasLocalFile || rowHover.containsMouse)
                            Layout.alignment: Qt.AlignVCenter
                            text: hasLocalFile ? "重新选择" : "选择文件"
                            font.pixelSize: 12
                            color: Style.accent
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.localFileRequested(itemId)
                            }
                        }

                        // 删除：只有房主，且悬停在这一行时才出现，平时不占视觉
                        Text {
                            visible: roomSession.isHost && rowHover.containsMouse
                            Layout.alignment: Qt.AlignVCenter
                            text: "✕"
                            font.pixelSize: 12
                            color: Style.textFaint
                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: roomSession.removeFromPlaylist(itemId)
                            }
                        }
                    }

                    // 下载进度条：贴在行内容下沿（状态文字里另有百分比与速度）
                    Rectangle {
                        visible: playlistTab.isUrlMode && isCurrent
                                 && playbackSync.downloadStage === "downloading"
                                 && playbackSync.downloadProgress >= 0
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: 51
                        anchors.leftMargin: 10
                        anchors.rightMargin: 14
                        height: 3
                        radius: 1.5
                        color: Style.fieldBg

                        Rectangle {
                            width: parent.width * playbackSync.downloadProgress / 100
                            height: parent.height
                            radius: parent.radius
                            color: Style.accent
                        }
                    }

                    Column {
                        id: errorColumn
                        visible: showError
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: 50
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        spacing: 4

                        Text {
                            width: parent.width
                            text: playbackSync.downloadError
                            wrapMode: Text.Wrap
                            font.pixelSize: 11
                            color: Style.danger
                        }

                        Text {
                            width: parent.width
                            text: "若反复失败，请更新 yt-dlp（命令见 tools/README.md）"
                            wrapMode: Text.Wrap
                            font.pixelSize: 11
                            color: Style.textFaint
                        }
                    }
                }
            }

            // 空态
            Column {
                anchors.centerIn: parent
                width: 200
                spacing: 10
                visible: roomSession.playlist.count === 0

                AppIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 34
                    height: 34
                    name: "film"
                    color: Style.border
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.canAddToPlaylist ? "播放列表还是空的"
                                                : "共享模式播放列表由房主管理"
                    font.pixelSize: 13
                    color: Style.textFaint
                }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: root.canAddToPlaylist ? "添加一部影片，大家一起看"
                                                : ""
                    font.pixelSize: 11
                    color: Style.textFaint
                }
            }
        }

        // ── 成员 ────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.currentTab === "members"

            ListView {
                id: memberList
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 6
                anchors.bottom: roomFoot.top
                clip: true
                spacing: 2
                model: roomSession.members

                delegate: Item {
                    width: memberList.width
                    height: 52

                    Row {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 14
                        anchors.rightMargin: 14
                        spacing: 9

                        Rectangle {
                            width: 34
                            height: 34
                            radius: 17
                            color: isHost ? Style.accent : Style.accentSoft
                            opacity: online ? 1.0 : 0.55

                            Text {
                                anchors.centerIn: parent
                                text: nickname.length > 0 ? nickname.charAt(0) : "?"
                                font.pixelSize: 13
                                font.bold: true
                                color: isHost ? "#FFFFFF" : Style.accent
                            }
                        }

                        Column {
                            // 34 头像 + 两侧各 9 间距 + 28 邮件按钮
                            width: parent.width - 34 - 9 - 9 - 28
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3

                            Row {
                                spacing: 5

                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: nickname
                                    font.pixelSize: 13
                                    color: Style.textPrimary
                                }

                                // 在线状态点：断线等待重连时变灰
                                Rectangle {
                                    visible: clientId !== roomSession.clientId
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 6
                                    height: 6
                                    radius: 3
                                    color: online ? Style.success : Style.textFaint
                                }

                                // 房主用品牌粉，和主色区分开
                                Rectangle {
                                    visible: isHost
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: hostText.implicitWidth + 8
                                    height: 15
                                    radius: Style.radiusTiny
                                    color: Style.brandSoft

                                    Text {
                                        id: hostText
                                        anchors.centerIn: parent
                                        text: "房主"
                                        font.pixelSize: 10
                                        font.bold: true
                                        color: Style.brand
                                    }
                                }

                                Rectangle {
                                    visible: clientId === roomSession.clientId
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: meText.implicitWidth + 8
                                    height: 15
                                    radius: Style.radiusTiny
                                    color: Style.fieldBg

                                    Text {
                                        id: meText
                                        anchors.centerIn: parent
                                        text: "你"
                                        font.pixelSize: 10
                                        color: Style.textSecondary
                                    }
                                }

                                Rectangle {
                                    visible: online && !loaded
                                             && roomSession.roomMode === RoomSession.Sync
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: warnText.implicitWidth + 8
                                    height: 15
                                    radius: Style.radiusTiny
                                    color: "#FFECEC"

                                    Text {
                                        id: warnText
                                        anchors.centerIn: parent
                                        text: "未加载"
                                        font.pixelSize: 10
                                        color: Style.danger
                                    }
                                }
                            }

                            Text {
                                text: !online
                                      ? "连接中断，等待重连…"
                                      : (!loaded && roomSession.roomMode === RoomSession.Sync
                                         ? "等待加载视频…" : "已加载视频")
                                font.pixelSize: 11
                                color: online ? Style.textFaint : Style.brand
                            }
                        }

                        // 邮件按钮：打开与这位成员的文件传输窗口。
                        // 自己不显示 —— 没有可用的对端连接。
                        Item {
                            id: mailButton
                            width: 28
                            height: 28
                            visible: clientId !== roomSession.clientId
                            anchors.verticalCenter: parent.verticalCenter

                            Rectangle {
                                anchors.fill: parent
                                radius: Style.radiusSmall
                                color: mailHover.containsMouse ? Style.fieldHover
                                                              : "transparent"
                            }

                            AppIcon {
                                anchors.centerIn: parent
                                width: 16
                                height: 16
                                name: "mail"
                                color: mailHover.containsMouse ? Style.accent
                                                               : Style.textSecondary
                            }

                            // 有待接收的文件时亮红点，窗口关着也不会漏掉
                            Rectangle {
                                visible: pendingFiles > 0
                                width: 14
                                height: 14
                                radius: 7
                                anchors.right: parent.right
                                anchors.top: parent.top
                                color: Style.danger

                                Text {
                                    anchors.centerIn: parent
                                    text: pendingFiles > 9 ? "9+" : pendingFiles
                                    font.pixelSize: 9
                                    color: "#FFFFFF"
                                }
                            }

                            MouseArea {
                                id: mailHover
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.fileTransferRequested(clientId, nickname)
                            }

                            ToolTip.visible: mailHover.containsMouse
                            ToolTip.text: pendingFiles > 0 ? "有文件待接收" : "文件传输"
                            ToolTip.delay: 500
                        }
                    }
                }
            }


            Rectangle {
                id: roomFoot
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 48

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 1
                    color: Style.border
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    spacing: 6

                    Text {
                        text: "房间号"
                        font.pixelSize: 12
                        color: Style.textFaint
                    }

                    Text {
                        text: roomSession.roomId
                        font.pixelSize: 13
                        font.bold: true
                        color: Style.textPrimary
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        id: copyHint
                        text: "复制邀请链接"
                        font.pixelSize: 12
                        color: Style.accent

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                clipboard.text = "房间号 " + roomSession.roomId
                                clipboard.selectAll()
                                clipboard.copy()
                                copyHint.text = "已复制"
                                copyReset.restart()
                            }
                        }
                    }
                }
            }

            Timer {
                id: copyReset
                interval: 1500
                onTriggered: copyHint.text = "复制邀请链接"
            }
        }
    }

    // ── 页签按钮（只有这个文件用到，就近定义） ──────────
    component PanelTab: Item {
        id: tab

        property string text: ""
        property int count: -1
        property bool selected: false
        signal clicked()

        width: tabRow.implicitWidth
        height: 40

        Row {
            id: tabRow
            anchors.centerIn: parent
            spacing: 5

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: tab.text
                font.pixelSize: 14
                font.bold: tab.selected
                color: tab.selected ? Style.textPrimary
                                    : (tabHover.containsMouse ? Style.accent : Style.textSecondary)
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                // visible: tab.count >= 0
                visible: false
                text: tab.count
                font.pixelSize: 11
                color: tab.selected ? Style.accent : Style.textFaint
            }
        }

        Rectangle {
            visible: tab.selected
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            width: 20
            height: 2
            color: Style.accent
        }

        MouseArea {
            id: tabHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: tab.clicked()
        }
    }
}
