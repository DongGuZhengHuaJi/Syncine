import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SyncineApp

// 网链模式:往播放列表里加一条的窗口。
//
// 两页,靠 page 切换:
//   第 0 页 选来源 —— 现在只有"自定义链接"一张卡,以后 bilibili/youtube 图标
//                     就是往这儿再加几张(每张卡带上自己的搜索前缀/解析方式)
//   第 1 页 粘链接 —— 解析 → 显示标题/时长/上传者 → 选画质 → 加入播放列表
//
// 三处容易踩的地方(改动时别踩回去):
//   1. 坐标基准是"声明它的那个 Item" —— 这个窗口挂在 RoomPage 根元素里(它铺满
//      窗口)才居中;挪到 FunctionPanel 里就会以 336 宽的侧栏为基准偏到左边。
//   2. 两页用 visible 切换而不是 StackLayout —— StackLayout 是"填满型"的,
//      不产生隐式高度,放进 Popup 会塌成 0。Positioner 会忽略不可见子项,
//      所以下面的 Column 高度天然只算当前页,窗口高度自己就对了。
//   3. qualities 是 QVariantList,delegate 里**只有 modelData**,没有角色名
//      (那是 QAbstractListModel 才有的);而且画质行数很少,用 Repeater + Column
//      就够了 —— ListView 是 Flickable,没有隐式高度,塞进这里会整块消失。
Popup {
    id: root

    // 0 = 选择来源,1 = 粘贴链接
    property int page: 0
    // 选中的画质档位(高度上限,0 = 最高画质)
    property int selectedHeight: 0
    // 这一份解析结果是哪个链接的 —— 用户改过输入框之后,旧结果就不该能提交
    property string inspectedUrl: ""

    readonly property string trimmedUrl: urlField.text.trim()
    // 只接受完整链接(关键词搜索以后再说):必须带 http(s):// 前缀
    readonly property bool urlLooksComplete: /^https?:\/\/\S+/i.test(trimmedUrl)
    readonly property bool probing: urlDownloader.stage === "probing"
    readonly property bool probed: urlDownloader.stage === "probed"
    readonly property bool canSubmit: probed && inspectedUrl !== "" && inspectedUrl === trimmedUrl

    modal: true
    focus: true
    // 点外面就关会把刚粘的链接和解析结果一起丢掉,只留 Esc
    closePolicy: Popup.CloseOnEscape

    width: 500
    // 内边距写在 Popup 上,不要写在 contentItem 里:
    // Control 会把 contentItem 的宽高设成「窗口尺寸 - 这里的内边距」,子项再用
    // width: parent.width 就正好贴合白卡片。反过来写在 contentItem 上,
    // 子项的 parent.width 会连内边距一起算进去 —— 右边整整多出一个 padding,
    // 戳出白卡片外面(表现就是"背景不够宽,选项超出去了")
    padding: 20

    // 显式居中(同 MessageDialog):默认 Popup 不保证居中
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)

    // 按设置里的"默认画质"挑一个**这条视频真的有**的档位。
    //
    // 不能直接把 defaultQuality 当选中值:档位是"高度上限",而下载时用的是
    // `bv*[height<=N]+ba/b[height<=N]` —— 如果 N 比这条视频最高画质还大,
    // 表达式可能一条都匹配不上、直接下载失败。所以取"不超过默认档的最大那个",
    // 实在没有(比如默认 360 但这条只有 1080)就退到最小的那个。
    function pickInitialQuality() {
        const want = appSettings.defaultQuality
        const list = urlDownloader.qualities
        if (want <= 0 || list.length === 0)
            return 0

        let best = -1
        let smallest = -1
        for (let i = 0; i < list.length; ++i) {
            const height = list[i].height
            if (height <= 0)
                continue
            if (height <= want && height > best)
                best = height
            if (smallest < 0 || height < smallest)
                smallest = height
        }
        return best > 0 ? best : (smallest > 0 ? smallest : 0)
    }

    function startProbe() {
        if (!urlLooksComplete || probing)
            return
        selectedHeight = 0            // 换了链接,上一轮的档位不作数
        inspectedUrl = trimmedUrl
        urlDownloader.inspect(trimmedUrl)
    }

    function submit() {
        if (!canSubmit)
            return
        // 参数必须**全部传满**:Q_INVOKABLE 的默认参数在 QML 里不存在,
        // 少传一个会直接报 "Insufficient arguments"
        roomSession.addUrl(trimmedUrl, urlDownloader.title, urlDownloader.live,
                           urlDownloader.durationMs,
                           urlDownloader.live ? 0 : selectedHeight)
        // 关掉就够 —— onClosed 里会 cancel() 把下载器的状态清干净
        root.close()
    }

    onOpened: {
        page = 0
        urlField.text = ""
        urlField.forceActiveFocus()
        selectedHeight = 0
        inspectedUrl = ""
    }
    onClosed: urlDownloader.cancel()

    // 解析出档位之后,把选中项落到"默认画质"对应的那一档上
    Connections {
        target: urlDownloader
        function onProbed() { root.selectedHeight = root.pickInitialQuality() }
    }

    background: Rectangle {
        radius: Style.radius
        color: Style.card
        border.width: 1
        border.color: Style.border
    }

    contentItem: Column {
        id: content

        // 这里不要写 padding,也不要绑 height ——
        // 内边距归 Popup(见上),而隐式高度正是这个窗口高度的来源
        spacing: 14

        // ── 头部:标题 + 关闭 ──
        Item {
            width: parent.width
            height: 22

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.page === 0 ? "添加视频" : "自定义链接"
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

        // ══════ 第 0 页:选来源 ══════
        Column {
            id: sourcePage
            width: parent.width
            spacing: 10

            visible: root.page === 0

            Text {
                width: parent.width
                text: "从哪里取视频"
                font.pixelSize: 12
                color: Style.textFaint
            }

            // 以后加 bilibili / youtube:这里再来一张同样的卡(或改 Repeater),
            // 卡片上带自己的图标和"输入什么"的说明
            Rectangle {
                id: customCard
                width: parent.width
                height: 58
                radius: Style.radius
                color: customHover.containsMouse ? Style.fieldHover : Style.fieldBg
                border.width: 1
                border.color: customHover.containsMouse ? Style.accent : Style.border

                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12

                    AppIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 18
                        height: 18
                        name: "link"
                        color: Style.accent
                    }

                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        Text {
                            text: "自定义链接"
                            font.pixelSize: 13
                            color: Style.textPrimary
                        }

                        Text {
                            text: "粘贴完整视频链接,可选画质"
                            font.pixelSize: 11
                            color: Style.textFaint
                        }
                    }
                }

                MouseArea {
                    id: customHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.page = 1
                        urlField.forceActiveFocus()
                    }
                }
            }
        }

        // ══════ 第 1 页:粘链接 + 挑画质 ══════
        Column {
            id: linkPage
            width: parent.width
            spacing: 10

            visible: root.page === 1

            Text {
                text: "‹ 选择其它来源"
                font.pixelSize: 12
                color: Style.accent

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.page = 0
                        urlDownloader.cancel()   // 换来源就别留着上次的解析结果
                    }
                }
            }

            // 外部工具没就位的话后面全是白搭,先说清缺什么
            Text {
                width: parent.width
                visible: !toolchain.ready
                text: "网链模式还差外部工具:" + toolchain.statusText
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: Style.danger
            }

            RowLayout {
                width: parent.width
                spacing: 8

                TextField {
                    id: urlField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    leftPadding: 10
                    rightPadding: 10
                    font.pixelSize: 12
                    color: Style.textPrimary
                    placeholderText: "粘贴完整链接(https://…)"
                    placeholderTextColor: Style.textFaint
                    selectByMouse: true
                    // 解析中不让改:结果和链接对不上会让人困惑
                    enabled: !root.probing

                    background: Rectangle {
                        radius: Style.radiusSmall
                        color: Style.card
                        border.width: 1
                        border.color: urlField.activeFocus ? Style.accent : Style.border
                    }

                    Keys.onReturnPressed: root.startProbe()
                }

                PrimaryButton {
                    Layout.preferredWidth: 60
                    Layout.preferredHeight: 34
                    font.pixelSize: 12
                    text: root.probing ? "…" : "解析"
                    enabled: root.urlLooksComplete && !root.probing
                    onClicked: root.startProbe()
                }
            }

            // 失败:yt-dlp 的输出原样显示 —— 它通常直接说清原因
            // (需要 cookie、要登录、版本太旧),比自己编一句有用
            Text {
                width: parent.width
                visible: urlDownloader.stage === "failed"
                text: urlDownloader.errorText
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: Style.danger
            }

            // 解析结果:标题 + 时长/直播 + 上传者(纯文字,不做缩略图)
            Column {
                width: parent.width
                spacing: 4

                visible: root.probed

                Text {
                    width: parent.width
                    text: urlDownloader.title
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    font.pixelSize: 14
                    color: Style.textPrimary
                }

                Text {
                    width: parent.width
                    font.pixelSize: 12
                    color: Style.textFaint
                    text: {
                        const parts = []
                        if (urlDownloader.live)
                            parts.push("直播")
                        else if (urlDownloader.durationMs > 0)
                            parts.push(Style.formatTime(urlDownloader.durationMs))
                        if (urlDownloader.uploader !== "")
                            parts.push(urlDownloader.uploader)
                        return parts.join(" · ")
                    }
                }
            }

            // 画质:直播没有档位(清单由播放器自适应),整块不显示
            Column {
                id: qualityList
                width: parent.width
                spacing: 4

                visible: root.probed && !urlDownloader.live

                Text {
                    text: "画质"
                    font.pixelSize: 12
                    color: Style.textSecondary
                }

                Repeater {
                    model: urlDownloader.qualities

                    delegate: Rectangle {
                        // QVariantList 的每一项通过 modelData 暴露(没有角色名)
                        required property var modelData

                        width: qualityList.width
                        height: 32
                        radius: Style.radiusSmall
                        border.width: 1
                        color: root.selectedHeight === modelData.height
                               ? Style.accentSoft : "transparent"
                        border.color: root.selectedHeight === modelData.height
                                      ? Style.accent : Style.border

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            font.pixelSize: 12
                            color: root.selectedHeight === modelData.height
                                   ? Style.accent : Style.textSecondary
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.selectedHeight = modelData.height
                        }
                    }
                }
            }

            PrimaryButton {
                width: parent.width
                text: "加入播放列表"
                enabled: root.canSubmit
                onClicked: root.submit()
            }
        }
    }
}
