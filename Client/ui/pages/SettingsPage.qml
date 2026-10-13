pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SyncineApp

// 全局设置页(从首页右上角的齿轮进来)。
//
// 两栏:左边分类、右边当前分类的内容。分类用下面的 sections 列表声明 ——
// 加一类就是加一行 + 写一张 Card,不用动布局。
//
// 一条约定:**这里的每一项都是"立刻生效"的** —— 控件直接读/写 appSettings
// 的属性,不需要"保存"按钮。QML 里属性赋值(appSettings.logLevel = x)和
// 调 setter(appSettings.setLogLevel(x))都可以,后者要记得 C++ 那边标了
// Q_INVOKABLE(否则会抛 "is not a function",还会中断同一处后面的语句)。
//
// `pragma ComponentBehavior: Bound` 是为了让左侧分类项能读到 root.section
// (不写的话能用,但 qmllint 会提示"嵌套组件里访问外部 id")。
Item {
    id: root

    // 当前分类,取 sections 里的 key
    property string section: "general"

    // cookie 卡片里选中的浏览器(记住上次用的,但要和当前探测结果核对 ——
    // 用户可能把浏览器卸了)
    property string cookieBrowser: ""

    // 进"播放与下载"时重新探测一次浏览器(装了新的不用重启程序)
    onSectionChanged: if (section === "playback") refreshCookieBrowsers()

    function refreshCookieBrowsers() {
        cookieManager.refreshBrowsers()
        const list = cookieManager.browsers
        const remembered = appSettings.lastCookieBrowser
        let found = false
        for (let i = 0; i < list.length; ++i) {
            if (list[i].value === remembered)
                found = true
        }
        cookieBrowser = found ? remembered
                              : (list.length > 0 ? list[0].value : "")
    }

    function cookieStatusText() {
        if (!cookieManager.ready)
            return "未设置"
        const parts = ["已导入 " + cookieManager.cookieCount + " 条"]
        if (cookieManager.source !== "")
            parts.push("来自 " + cookieManager.source)
        if (cookieManager.importedAt !== "")
            parts.push(Qt.formatDateTime(new Date(cookieManager.importedAt), "yyyy-MM-dd HH:mm"))
        return parts.join(" · ")
    }

    readonly property var sections: [
        { "key": "general", "label": "通用" },
        { "key": "playback", "label": "播放与下载" },
        { "key": "audio", "label": "音频" },
        { "key": "log", "label": "日志与诊断" }
    ]

    // 保存地址 → 落盘 → 立刻按新地址重连。
    // 不做"边打字边连":那会在输入的每个字符上重连一次
    function applyServerUrl() {
        appSettings.setServerUrl(serverField.text.trim())
        networkManager.connectToServer()
    }

    function serverStatusText() {
        if (networkManager.isConnected)
            return "已连接:" + networkManager.effectiveUrl
        if (networkManager.connecting)
            return "连接中…"
        if (networkManager.retrying)
            return "未连接,正在自动重试"
        return "未连接"
    }

    // 左侧分类项。用内联组件(和 FunctionPanel 的 PanelTab 一个路子)
    component SectionItem: Rectangle {
        required property var modelData

        width: sidebar.width
        height: 34
        radius: Style.radiusSmall
        color: root.section === modelData.key ? Style.accentSoft
             : (sectionHover.containsMouse ? Style.hover : "transparent")

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            text: modelData.label
            font.pixelSize: 13
            color: root.section === modelData.key ? Style.accent : Style.textSecondary
        }

        MouseArea {
            id: sectionHover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.section = modelData.key
        }
    }

    // ══ 右侧:当前分类的内容(可滚动)══
    Flickable {
        id: contentFlick
        anchors.left: parent.left
        anchors.leftMargin: 190
        anchors.right: parent.right
        anchors.rightMargin: 28
        anchors.top: parent.top
        anchors.topMargin: 76
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: contentColumn.height + 40
        clip: true

        Column {
            id: contentColumn
            width: Math.min(parent.width, 560)
            spacing: 12

            // ── 通用:服务器 ──────────────────────
            Card {
                width: parent.width
                visible: root.section === "general"

                Column {
                    width: parent.width
                    spacing: 10

                    Label {
                        text: "服务器地址"
                        font.pixelSize: 15
                        font.bold: true
                        color: Style.textPrimary
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.textFaint
                        text: "留空就用内置地址 " + appSettings.fallbackServerUrl
                              + "。填了你自己的服务器之后,连不上会自动回退到内置地址"
                    }

                    RowLayout {
                        width: parent.width
                        spacing: 8

                        TextField {
                            id: serverField
                            Layout.fillWidth: true
                            Layout.preferredHeight: 34
                            leftPadding: 10
                            rightPadding: 10
                            font.pixelSize: 12
                            color: Style.textPrimary
                            selectByMouse: true
                            placeholderText: appSettings.fallbackServerUrl
                            text: appSettings.serverUrl
                            Keys.onReturnPressed: root.applyServerUrl()

                            background: Rectangle {
                                radius: Style.radiusSmall
                                color: Style.card
                                border.width: 1
                                border.color: serverField.activeFocus ? Style.accent : Style.border
                            }
                        }

                        PrimaryButton {
                            Layout.preferredWidth: 72
                            Layout.preferredHeight: 34
                            font.pixelSize: 12
                            text: "保存"
                            onClicked: root.applyServerUrl()
                        }
                    }

                    RowLayout {
                        width: parent.width
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            font.pixelSize: 12
                            color: networkManager.isConnected ? Style.success
                                 : networkManager.retrying ? Style.danger
                                 : Style.textSecondary
                            text: root.serverStatusText()
                        }

                        Text {
                            text: "立即重试"
                            font.pixelSize: 12
                            color: Style.accent

                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: networkManager.retryNow()
                            }
                        }
                    }
                }
            }

            // ── 播放与下载:画质 ──────────────────
            Card {
                width: parent.width
                visible: root.section === "playback"

                Column {
                    width: parent.width
                    spacing: 10

                    Label {
                        text: "默认画质"
                        font.pixelSize: 15
                        font.bold: true
                        color: Style.textPrimary
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.textFaint
                        text: "网链模式加条目时的默认档位(1080 = 不超过 1080P,"
                              + "下载时按\"分辨率优先、H.264 优先、AAC 优先\"的规则挑选)。"
                              + "条目上会记下当时选的档位,改这里不影响已经加好的条目"
                    }

                    OptionList {
                        // value 是 spec 的"主体"("" = 最高画质,见 FormatModel.h)
                        options: [
                            { label: "最高画质", value: "" },
                            { label: "2160P(4K)", value: "2160" },
                            { label: "1440P(2K)", value: "1440" },
                            { label: "1080P", value: "1080" },
                            { label: "720P", value: "720" },
                            { label: "480P", value: "480" },
                            { label: "360P", value: "360" }
                        ]
                        selectedValue: appSettings.defaultQuality
                        onPicked: (value) => appSettings.setDefaultQuality(value)
                    }

                    Label {
                        width: parent.width
                        topPadding: 6
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.textFaint
                        text: "动态范围(HDR / 杜比视界)。开着的话,"
                              + "画质档会优先挑带 HDR 的流 —— 下载没问题,"
                              + "但当前播放器要先离线转成 SDR 才能正常显示(亮处会过曝);"
                              + "关了则只挑 SDR 流。详情视图里看到的 HDR 标不受这个开关限制"
                    }

                    OptionList {
                        options: [
                            { label: "优先 SDR(推荐)", value: false },
                            { label: "允许 HDR", value: true }
                        ]
                        selectedValue: appSettings.hdrAllowed
                        onPicked: (value) => appSettings.setHdrAllowed(value)
                    }
                }
            }

            // ── 播放与下载:Cookie ────────────────
            Card {
                width: parent.width
                visible: root.section === "playback"

                Column {
                    width: parent.width
                    spacing: 10

                    Label {
                        text: "Cookie"
                        font.pixelSize: 15
                        font.bold: true
                        color: Style.textPrimary
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.textFaint
                        text: "下载需要登录的站点时,让 yt-dlp 带上你的登录态。"
                              + "从浏览器导入会把该浏览器里的「全部」cookie 导到本机一个文件里"
                              + "(只有你自己的账号能读)。用完之后可以在这里清除"
                    }

                    // 状态行
                    Row {
                        spacing: 6

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 8
                            height: 8
                            radius: 4
                            color: cookieManager.ready ? Style.success : Style.textSecondary
                        }

                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.cookieStatusText()
                            font.pixelSize: 12
                            color: cookieManager.ready ? Style.textSecondary : Style.textFaint
                        }
                    }

                    // 导进来的快照会过期,超过一个月提醒一次
                    Label {
                        width: parent.width
                        visible: cookieManager.stale
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.danger
                        text: "这份 cookie 已经超过一个月没更新了。下载时提示需要登录的话,重新导入一次"
                    }

                    Label {
                        width: parent.width
                        visible: cookieManager.browsers.length > 0
                        text: "从浏览器导入"
                        font.pixelSize: 12
                        color: Style.textSecondary
                    }

                    OptionList {
                        visible: cookieManager.browsers.length > 0
                        options: cookieManager.browsers
                        selectedValue: root.cookieBrowser
                        onPicked: (value) => root.cookieBrowser = value
                    }

                    Label {
                        width: parent.width
                        visible: cookieManager.browsers.length === 0
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.textFaint
                        text: "没检测到常见浏览器(检测不到也能用 —— 可以「从文件导入」)"
                    }

                    RowLayout {
                        width: parent.width
                        spacing: 8

                        PrimaryButton {
                            Layout.preferredWidth: 96
                            Layout.preferredHeight: 34
                            font.pixelSize: 12
                            text: cookieManager.busy ? "导入中…" : "导入"
                            enabled: !cookieManager.busy
                                     && cookieManager.browsers.length > 0
                                     && toolchain.ytDlpReady
                            onClicked: cookieManager.importFromBrowser(root.cookieBrowser)
                        }

                        GhostButton {
                            text: "从文件导入…"
                            enabled: !cookieManager.busy
                            onClicked: cookieFileDialog.open()
                        }

                        Item { Layout.fillWidth: true }

                        Text {
                            text: "清除"
                            font.pixelSize: 12
                            color: cookieManager.ready && !cookieManager.busy
                                   ? Style.danger : Style.textFaint
                            visible: cookieManager.ready

                            MouseArea {
                                anchors.fill: parent
                                anchors.margins: -6
                                cursorShape: Qt.PointingHandCursor
                                onClicked: if (!cookieManager.busy) cookieManager.clear()
                            }
                        }
                    }

                    // 失败原因(已经在 C++ 里翻译成人话了)
                    Label {
                        width: parent.width
                        visible: cookieManager.errorText !== ""
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.danger
                        text: cookieManager.errorText
                    }
                }
            }

            // ── 音频(阶段三做,先占个位)─────────
            Card {
                width: parent.width
                visible: root.section === "audio"

                Column {
                    width: parent.width
                    spacing: 10

                    Label {
                        text: "音频设备"
                        font.pixelSize: 15
                        font.bold: true
                        color: Style.textPrimary
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.textFaint
                        text: "麦克风与听筒的设备选择正在做。做好之后这一页会出现两个设备列表,"
                              + "房间里的设置面板也会有一份同样的(房间内只能改这两项)"
                    }
                }
            }

            // ── 日志与诊断:级别 + 运行环境 ────────
            Card {
                width: parent.width
                visible: root.section === "log"

                Column {
                    width: parent.width
                    spacing: 10

                    Label {
                        text: "日志级别"
                        font.pixelSize: 15
                        font.bold: true
                        color: Style.textPrimary
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.textFaint
                        text: "级别是「从这一档往上都记」:选 warn 就只剩警告和错误。"
                              + "排查问题时调成「调试」,调完记得改回来 —— 级别越细日志越大。"
                              + "(环境变量 SYNCINE_LOG_LEVEL 存在时会盖过这里)"
                    }

                    OptionList {
                        options: [
                            { label: "最详细(trace)", value: "trace" },
                            { label: "调试(debug)", value: "debug" },
                            { label: "常规(info)", value: "info" },
                            { label: "只看警告(warn)", value: "warn" },
                            { label: "只看错误(error)", value: "error" }
                        ]
                        selectedValue: appSettings.logLevel
                        onPicked: (value) => appSettings.setLogLevel(value)
                    }
                }
            }

            Card {
                width: parent.width
                visible: root.section === "log"

                Column {
                    width: parent.width
                    spacing: 8

                    Label {
                        text: "运行环境"
                        font.pixelSize: 15
                        font.bold: true
                        color: Style.textPrimary
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.textSecondary
                        text: "外部工具:" + toolchain.statusText
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.textFaint
                        text: "配置文件:" + appSettings.filePath()
                    }

                    Label {
                        width: parent.width
                        wrapMode: Text.Wrap
                        font.pixelSize: 12
                        color: Style.textFaint
                        text: "日志目录:" + appSettings.logDirectory()
                    }

                    GhostButton {
                        text: "打开日志目录"
                        onClicked: appSettings.openLogDirectory()
                    }
                }
            }

            Item { width: 1; height: 24 }
        }
    }

    // ══ 左侧:标题 + 分类 ══
    Label {
        anchors.left: parent.left
        anchors.leftMargin: 28
        anchors.top: parent.top
        anchors.topMargin: 76
        text: "设置"
        font.pixelSize: 26
        font.bold: true
        color: Style.textPrimary
    }

    Column {
        id: sidebar
        anchors.left: parent.left
        anchors.leftMargin: 28
        anchors.top: parent.top
        anchors.topMargin: 120
        width: 134
        spacing: 2

        Repeater {
            model: root.sections
            delegate: SectionItem {}
        }
    }

    FileDialog {
        id: cookieFileDialog
        title: "选择 cookies.txt"
        nameFilters: ["Cookie 文件 (*.txt)", "所有文件 (*)"]
        // selectedFile 是 url —— C++ 那边收的就是 QUrl(直接当字符串用会拿到 "file:///...")
        onAccepted: cookieManager.importFromFile(selectedFile)
    }

    // ⚠️ 返回按钮**必须定义在 Flickable 后面**(QML 里后声明的兄弟节点在上面,
    // 也先拿到鼠标事件)。放在前面的话,铺满整页的 Flickable 会把手按事件
    // 抢走(它要靠拖动才能滚动),按钮点了完全没反应 —— 而且**不报任何错**。
    GhostButton {
        anchors.left: parent.left
        anchors.leftMargin: 28
        anchors.top: parent.top
        anchors.topMargin: 24
        text: "← 返回"
        onClicked: stackView.pop()
    }
}
