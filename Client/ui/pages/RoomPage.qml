import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import SyncineApp

// 房间页:顶栏 / 播放器 / 右侧功能框。
//
// 本页只做展示和转达用户意图,并且持有"房间内所有消息"这一份数据 ——
// 弹幕和评论区共用它(见 sendMessage)。播放广播、加载门禁、回声抑制
// 都在 PlaybackSync 里,这一层不再判断。
Item {
    id: root

    property bool leaving: false

    // normal 普通 / theater 宽屏 / winfull 窗口全屏 / screenfull 屏幕全屏
    property string viewMode: "normal"
    property bool danmakuVisible: true

    // 进屏幕全屏前的窗口状态,退出时还原(否则会把最大化也一起弄丢)
    property int visibilityBeforeFullscreen: Window.Windowed

    readonly property bool fullscreenStyle: viewMode === "winfull" || viewMode === "screenfull"
    // 顶栏:窗口全屏和屏幕全屏都收起
    readonly property bool chromeVisible: viewMode === "normal" || viewMode === "theater"

    // 这一端"需要自己加载片子"吗:本地模式是人人都要,共享模式只有房主要
    //(观众的画面是房主推过来的,他们本机没有文件)
    readonly property bool needsLocalFile:
        roomSession.roomMode === RoomSession.Sync
        || (roomSession.roomMode === RoomSession.Share && roomSession.isHost)

    // 自己这边还没有可播的片子 —— 这时候点画面 = 去选文件,而不是播放/暂停。
    // 用 hasLoaded 而不是"条目有没有映射":文件被删/拷走后映射还在,
    // 那种情况下点画面也该能重新选一个
    readonly property bool awaitingFile:
        needsLocalFile && !playbackController.hasLoaded

    // ── 消息 ────────────────────────────────────────────
    // 弹幕和评论是同一份数据,只有这一个 model
    ListModel {
        id: messageModel
        ListElement { who: "系统"; text: "欢迎来到房间,把房间号发给好友一起观影" }
    }

    // 只有真正"进评论列表"的消息才需要出现在列表里;
    // 弹幕层是从同一条消息派生的,所以这里顺手也推一条弹幕。
    function sendMessage(text) {
        const t = text.trim()
        if (t === "")
            return
        messageModel.append({ "who": sessionController.nickname, "text": t })
        // 自己的弹幕要自己补:服务端广播时把发送者排除了(LogicSystem.cpp),
        // 不会回发给我
        videoPlayer.pushDanmaku(t)
        roomSession.sendChat(t)
    }

    // 别人发来的:进列表 + 飘弹幕
    function receiveMessage(who, text) {
        messageModel.append({ "who": who, "text": text })
        videoPlayer.pushDanmaku(text)
    }

    function pushSystem(text) {
        messageModel.append({ "who": "系统", "text": text })
    }

    // ── 形态切换 ────────────────────────────────────────
    function setViewMode(mode) {
        if (mode === root.viewMode)
            return

        const win = root.Window.window

        // 离开屏幕全屏:先把窗口还原再切
        if (root.viewMode === "screenfull" && win)
            win.visibility = root.visibilityBeforeFullscreen

        if (mode === "screenfull" && win) {
            root.visibilityBeforeFullscreen = win.visibility
            win.visibility = Window.FullScreen
        }

        root.viewMode = mode
    }

    function leaveCurrentRoom() {
        if (leaving)
            return
        leaving = true

        // 全屏状态下离开:先把窗口还原,不然回到首页还是全屏的
        const win = root.Window.window
        if (win && win.visibility === Window.FullScreen)
            win.visibility = root.visibilityBeforeFullscreen

        // 先把麦克风关掉再走。否则人已经离开房间了,麦克风还在采集 ——
        // 系统托盘上的录音指示灯会一直亮着,是用户最先注意到的异常。
        signalingChannel.setAudioEnabled(false)
        playbackController.pause()
        // 把已经挂上的片子卸掉:房间都退了,本地播放器还占着文件没有意义,
        // 回到首页再进别的房间时也不该看到上一场的画面
        playbackController.unload()
        sessionController.leaveRoom()
        stackView.pop()
    }

    // 选文件:targetItemId 为空 = 新加一条;非空 = 给这一条指定本机文件
    function requestLocalFile(targetItemId) {
        fileDialog.targetItemId = targetItemId
        fileDialog.open()
    }

    // 点画面 / 点播放列表里的"添加" —— 都走这里
    function requestNewItem() {
        const current = roomSession.playlist.currentItemId
        const currentReady = roomSession.playlist.currentReady
        // 当前条目缺本机文件时,选文件是"补给它"而不是"加一条新的"
        if (needsLocalFile && current !== "" && !currentReady) {
            requestLocalFile(current)
            return
        }
        requestLocalFile("")
    }

    // 窗口自己脱离了全屏(比如被窗口管理器改了),界面状态要跟上
    Connections {
        target: root.Window.window
        function onVisibilityChanged() {
            if (root.viewMode === "screenfull"
                    && root.Window.window.visibility !== Window.FullScreen)
                root.viewMode = "normal"
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.viewMode === "screenfull"
        onActivated: root.setViewMode("normal")
    }

    MessageDialog {
        id: messageDialog
    }

    FileDialog {
        id: fileDialog

        // 给哪一条指定文件。空串 = 作为新条目加进播放列表
        property string targetItemId: ""

        title: targetItemId === "" ? "选择视频文件" : "这个条目对应本机的哪个文件"
        nameFilters: ["视频文件 (*.mp4 *.mkv *.avi *.mov *.webm *.flv)", "所有文件 (*)"]

        onAccepted: {
            if (targetItemId === "")
                roomSession.addLocalFile(selectedFile)
            else
                roomSession.assignLocalFile(targetItemId, selectedFile)
        }
    }

    // 房间级的错误(比如非房主想切集、共享模式下想添加)统一在这里弹
    Connections {
        target: sessionController
        function onErrorOccurred(message) {
            messageDialog.message = message
            messageDialog.open()
        }
    }

    Connections {
        target: roomSession

        function onChatReceived(from, text) {
            root.receiveMessage(from, text)
        }

        function onMemberJoined(nickname) {
            root.pushSystem(nickname + " 加入了房间")
        }

        function onMemberLeft(nickname) {
            root.pushSystem(nickname + " 离开了房间")
        }

        function onVideoMismatchChanged() {
            if (roomSession.videoMismatched) {
                messageDialog.message = "检测到成员加载的视频不一致,同步将适配较短视频的时长"
                messageDialog.open()
            }
        }

        // 服务端断开(或被动离开)时,弹回上一页
        function onLeft() {
            if (leaving)
                return
            const win = root.Window.window
            if (win && win.visibility === Window.FullScreen)
                win.visibility = root.visibilityBeforeFullscreen
            signalingChannel.setAudioEnabled(false)
            playbackController.pause()
            // 被动离开(被服务端断开 / 房间解散)也要把挂着的片子卸掉
            playbackController.unload()
            stackView.pop()
        }
    }

    // 麦克风状态不需要 Connections 同步 —— signalingChannel.audioEnabled
    // 是 Q_PROPERTY,引用它的绑定会随 NOTIFY 自动重算。

    // ══ 1. 顶栏 ══════════════════════════════════════════
    Rectangle {
        id: topBar

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Style.topBarHeight
        color: Style.card
        visible: root.chromeVisible

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
            spacing: 10

            Button {
                id: backBtn
                width: 30
                height: 30
                hoverEnabled: true

                contentItem: AppIcon {
                    anchors.centerIn: parent
                    width: 18
                    height: 18
                    name: "back"
                    color: backBtn.hovered ? Style.accent : Style.textSecondary
                }

                background: Rectangle {
                    radius: Style.radiusSmall
                    color: backBtn.hovered ? Style.fieldBg : "transparent"
                }

                ToolTip.visible: hovered
                ToolTip.text: "离开房间"
                ToolTip.delay: 500

                onClicked: root.leaveCurrentRoom()
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, 190)
                elide: Text.ElideRight
                text: roomSession.roomName !== "" ? roomSession.roomName
                                                  : ("房间 " + roomSession.roomId)
                font.pixelSize: 15
                font.bold: true
                color: Style.textPrimary
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: roomIdRow.implicitWidth + 16
                height: 20
                radius: Style.radiusSmall
                color: idHover.containsMouse ? Style.fieldHover : Style.fieldBg

                Row {
                    id: roomIdRow
                    anchors.centerIn: parent
                    spacing: 4

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "房间号 " + roomSession.roomId
                        font.pixelSize: 11
                        color: Style.textSecondary
                    }
                }

                MouseArea {
                    id: idHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        clipboard.text = roomSession.roomId
                        clipboard.selectAll()
                        clipboard.copy()
                    }
                }
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: onlineText.implicitWidth + 14
                height: 20
                radius: Style.radiusSmall
                color: Style.accentSoft

                Text {
                    id: onlineText
                    anchors.centerIn: parent
                    text: roomSession.members.count + " 人在线"
                    font.pixelSize: 11
                    color: Style.accent
                }
            }
        }

        // 模式选择(本地 / 共享 / 网链)
        Rectangle {
            id: modeSelector
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            width: modeRow.implicitWidth + 4
            height: 30
            radius: Style.radius
            color: Style.fieldBg

            Row {
                id: modeRow
                anchors.centerIn: parent
                spacing: 2

                ModeButton {
                    width: 74
                    text: "同步"
                    enabled: roomSession.isHost
                    selected: roomSession.roomMode === RoomSession.Sync
                    onClicked: roomSession.setRoomMode(RoomSession.Sync)
                }

                ModeButton {
                    width: 74
                    text: "共享"
                    enabled: roomSession.isHost
                    selected: roomSession.roomMode === RoomSession.Share
                    onClicked: roomSession.setRoomMode(RoomSession.Share)
                }

                ModeButton {
                    width: 74
                    text: "网链"
                    enabled: roomSession.isHost
                    selected: roomSession.roomMode === RoomSession.Url
                    onClicked: roomSession.setRoomMode(RoomSession.Url)
                }
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            Button {
                id: loadVideoBtn
                width: 85
                height: 30
                hoverEnabled: true

                contentItem: Row {
                    anchors.centerIn: parent
                    spacing: 5

                    AppIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        height: 14
                        name: "folder-plus"
                        color: "#FFFFFF"
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "加载视频"
                        font.pixelSize: 13
                        color: "#FFFFFF"
                    }
                }

                background: Rectangle {
                    radius: Style.radiusSmall
                    color: loadVideoBtn.pressed ? Style.accentPressed
                         : (loadVideoBtn.hovered ? Style.accentHover : Style.accent)
                }

                onClicked: root.requestNewItem()
            }
        }
    }

    // 顶栏里的房间号也要能复制
    TextField {
        id: clipboard
        visible: false
    }

    // ══ 2 + 3. 播放器 与 功能框 ═══════════════════════════
    Item {
        id: body

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.top: parent.top
        anchors.topMargin: topBar.visible ? Style.topBarHeight : 0
        anchors.leftMargin: root.fullscreenStyle ? 0 : 12
        anchors.rightMargin: root.fullscreenStyle ? 0 : 12
        anchors.bottomMargin: root.fullscreenStyle ? 0 : 12

        Row {
            anchors.fill: parent
            spacing: root.fullscreenStyle ? 0 : 10

            Rectangle {
                id: playerCard
                width: panel.visible ? parent.width - panel.width - 10 : parent.width
                height: parent.height
                color: Style.playerBg
                radius: root.fullscreenStyle ? 0 : Style.radius
                border.width: root.fullscreenStyle ? 0 : 1
                border.color: Style.border
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    VideoPlayer {
                        id: videoPlayer
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        fullscreenStyle: root.fullscreenStyle
                        viewMode: root.viewMode
                        danmakuVisible: root.danmakuVisible
                        hintText: root.hintMessage()
                        awaitingFile: root.awaitingFile

                        onDanmakuSubmitted: (text) => root.sendMessage(text)
                        onDanmakuToggleRequested: root.danmakuVisible = !root.danmakuVisible
                        onViewModeRequested: (mode) => root.setViewMode(mode)
                        onLoadRequested: root.requestNewItem()
                    }

                    // 非全屏时,弹幕输入框在播放器下方
                    DanmakuBar {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 50
                        visible: !root.fullscreenStyle
                        onSubmitted: (text) => root.sendMessage(text)
                    }
                }
            }

            FunctionPanel {
                id: panel
                width: 336
                height: parent.height
                visible: root.viewMode === "normal"
                messages: messageModel
                onCommentSubmitted: (text) => root.sendMessage(text)
                onAddRequested: root.requestNewItem()
                onLocalFileRequested: (itemId) => root.requestLocalFile(itemId)
            }
        }
    }

    // 画面中央的提示语(门禁状态由 PlaybackSync 判定,这里只负责翻译成文案)
    function hintMessage() {
        const mode = roomSession.roomMode

        if (mode === RoomSession.Sync) {
            if (playbackSync.blockReason === PlaybackSync.LocalNotLoaded) {
                // 已经有一条在等着了,那缺的是"这一条对应的本机文件";
                // 列表还是空的,才是"还没选过片子"
                return roomSession.playlist.currentItemId !== ""
                       ? "点击画面,选择这一条对应的本机文件"
                       : "点击画面,选择视频文件"
            }
            if (playbackSync.blockReason === PlaybackSync.OthersNotLoaded)
                return "等待其他成员加载视频…"
            return ""
        }

        if (mode === RoomSession.Share) {
            // 观众的画面来自房主,他们本地没什么可做的,不给提示;
            // 房主还没选片子才提示
            if (roomSession.isHost && !playbackController.hasLoaded)
                return "点击画面,选择要共享的视频"
            return ""
        }

        if (mode === RoomSession.Url)
            return "网链模式 · 开发中"

        return ""
    }
}
