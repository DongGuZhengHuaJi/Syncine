import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SyncineApp

// 网链模式：向播放列表添加条目的窗口。
//
// 两页，通过 page 切换：
//   第 0 页 选择来源 —— 目前仅有「自定义链接」一张卡，后续 bilibili/youtube
//                      图标即在此处追加（每张卡带上自己的搜索前缀与解析方式）
//   第 1 页 粘贴链接 —— 解析 → 标题/时长/上传者 → 选择画质（或展开详情逐条选流）
//
// 画质的两种视图（详细设计见 FormatModel.h 顶部的 spec）：
//   简单：仅列出分辨率档，选档即可——下载时按 `-S` 的优先级自动配编码与音频
//   详情：视频/音频/合并三类逐条列出（编码、码率、体积、HDR 均标出），
//         手选的一对 format_id 原样记入条目（spec），各端依此下载
//
// 三处约束（修改时需注意）：
//   1. 坐标基准是「声明它的那个 Item」——本窗口挂在 RoomPage 根元素下（铺满
//      窗口）才居中；移到 FunctionPanel 中会以 336 宽的侧栏为基准偏向左。
//   2. 两页用 visible 切换而非 StackLayout——StackLayout 是「填满型」的，
//      不产生隐式高度，放入 Popup 会塌陷为 0。Positioner 会忽略不可见子项，
//      故下方 Column 的高度天然只计当前页，窗口高度自动正确。
//   3. qualities / videoFormats 均为 QVariantList，delegate 中**只有
//      modelData**，没有角色名（那是 QAbstractListModel 才有的）。
Popup {
    id: root

    // 0 = 选择来源，1 = 粘贴链接
    property int page: 0
    // 详情视图（逐条选流）展开没有
    property bool detailMode: false
    // 这一份解析结果是哪个链接的 —— 用户改过输入框之后，旧结果就不该能提交
    property string inspectedUrl: ""
    // 用户选的是哪张来源卡："bilibili" / "generic"
    property string sourcePlatform: "generic"
    // 输入框原文（送给 probe 前用于判断「改没改过」）
    property string probedInput: ""
    // 规范化后的完整链接（条目上存的就是它，各端 itemId 才一致）
    property string normalizedUrl: ""
    property string normalizeError: ""
    // 剧集：用户点了第几集（用它去下面的 seasonEpisodes 里取 url/title/itemId;-1 = 没点）
    property int pickedEpisode: -1

    // ---- 整季的本地快照 ----
    // **必须保存这一份，不能直接读取 urlInspector.episodes**：点选某一集后探测器
    // 的 episodes 会被清空并替换为该集的 formats（这是有意为之），而「当前处于
    // 哪一层」「点选了哪一集」这些判断需始终成立——若以被清空的列表为依据，
    // 返回按钮将永不出现（实测）。
    property var seasonEpisodes: []
    property string seasonTitle: ""
    // 每一集各自选择过的画质：{ 该集的 url → spec }。
    // **按集存储**是本功能的要点：返回列表后再次进入、或「加入整季」时
    // 都需要它——只记录「当前这一条」的 spec 并不足够（实测反馈）。
    property var episodeSpecs: ({})

    readonly property string trimmedUrl: urlField.text.trim()
    // 通用卡要求完整链接；B 站卡还接受 BV/av/ss/ep/直播间号。
    readonly property bool urlLooksComplete: /^https?:\/\/\S+/i.test(trimmedUrl)
    readonly property bool inputLooksUsable: sourcePlatform === "bilibili"
                                             ? trimmedUrl.length > 0
                                             : urlLooksComplete
    readonly property bool probing: urlInspector.stage === "probing"
    // 注意**不可命名为 inspected**：那样会遮蔽 urlInspector 的 `inspected()` 信号名，
    // Connections 中的 onInspected 将永不触发（实测——表现为默认档位无法选中、
    // 加入按钮不可点击）。
    readonly property bool inspectedOk: urlInspector.stage === "inspected"
    readonly property bool live: inspectedOk && urlInspector.live
    // 现在展示的是整季列表（还是某一集的画质页）—— 由探测器的 playlistView 说了算
    readonly property bool isPlaylist: inspectedOk && urlInspector.playlistView
    // 已经点定某一集（它的画质档才在 qualities 里）
    readonly property bool episodePicked: pickedEpisode >= 0 && pickedEpisode < seasonEpisodes.length
                                          && urlInspector.qualities.length > 0
    readonly property var pickedEpisodeData: episodePicked ? seasonEpisodes[pickedEpisode] : null
    // 简单视图的档位行。**供 OptionList 使用，键名必须符合其约定**：
    //   { label(主文字), value(选中值，用于与 selectedValue 比较), detail(右上小字) }
    // C++ 侧提供的是 { label, spec, detail, ... }——spec 才是指向条目画质的取值，
    // 故在此转换为 value。**不要将原始数据直接传给 OptionList**：其 delegate 读取
    // modelData.value，键名不匹配即为 undefined（实测——点击有响应但取值丢失，
    // 表现为「怎么点都选不中」）。
    readonly property var qualityOptions: {
        if (!inspectedOk)
            return []

        var rows = []
        if (!live) {
            // 普通视频才有"不设限"这一档；直播的每条 variant 都是具体的流
            rows.push({ label: "最高画质", value: "0", detail: "" })
        }
        for (var i = 0; i < urlInspector.qualities.length; ++i) {
            var option = urlInspector.qualities[i]
            rows.push({ label: option.label, value: option.spec, detail: option.detail })
        }
        return rows
    }
    // 用 C++ 的 selectionSpec 属性（带 NOTIFY）而不是 buildSpec() 方法：
    // 方法调用不会因为信号被重新求值，按钮就永远亮不起来（同一个坑）
    //
    // 多集链接特殊：刚解析完只有剧集列表（没有档位）,必须先点定某一集才能加；
    // 而点某一集会再解析一次，那时 urlInspector 看的就是那一集了
    readonly property bool canSubmit: inspectedOk && urlInspector.selectionSpec !== ""
                                      && inspectedUrl !== "" && normalizedUrl !== ""
                                      && probedInput === trimmedUrl
                                      && !isPlaylist

    modal: true
    focus: true
    // 点外面就关会把刚粘的链接和解析结果一起丢掉，只留 Esc
    closePolicy: Popup.CloseOnEscape

    width: 500
    // 内边距写在 Popup 上，不要写在 contentItem 里：
    // Control 会把 contentItem 的宽高设成「窗口尺寸 - 这里的内边距」，子项再用
    // width: parent.width 就正好贴合白卡片。反过来写在 contentItem 上，
    // 子项的 parent.width 会连内边距一起算进去 —— 右边整整多出一个 padding,
    // 戳出白卡片外面（表现就是"背景不够宽，选项超出去了"）
    padding: 20

    // 显式居中（同 MessageDialog）:默认 Popup 不保证居中
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)

    // 档位 spec("1080"/"0"/空)拼上设置里的 HDR 开关。
    // 详情视图手选之后，档位选择就作废（两种语义互斥）
    function selectTier(spec) {
        // 这两行是**故意留着的**调试线索(console.log 是 debug 级，默认级别看不到，
        // 排查时用 SYNCINE_LOG_LEVEL=debug 启动):
        //   没有这两行         → 点击没送到选项行（高度/命中区域的问题）
        //   第一行是 undefined → 选项数据的键名和 OptionList 对不上（见 qualityOptions）
        //   两行都有但值不变   → 问题在 C++ 的选择逻辑
        console.log("选择档位:", JSON.stringify(spec), "  当前:", urlInspector.tierSpec)
        urlInspector.clearPicked()
        urlInspector.selectTier(spec)
        console.log("选择档位后 tierSpec =", urlInspector.tierSpec)
    }

    function detailPicked() {
        return urlInspector.pickedVideoId !== "" || urlInspector.pickedAudioId !== ""
    }

    // 换一个链接 → 一切从头来。
    // 先把平台编号/短链规范化为完整链接；条目上存的是 normalizedUrl。
    function startProbe() {
        if (!inputLooksUsable || probing)
            return

        normalizeError = ""
        // 通用卡不指定平台：注册表会先尝试 Bilibili（URL/编号都能命中），
        // 失败再落到 Generic。这样 B 站链接从哪张卡进来都会得到同一个规范化 URL。
        const platformId = sourcePlatform === "bilibili" ? "bilibili" : ""
        const result = mediaPlatforms.normalize(platformId, trimmedUrl)
        if (result.error !== "") {
            normalizeError = result.error
            normalizedUrl = ""
            return
        }

        probedInput = trimmedUrl
        normalizedUrl = result.url
        inspectedUrl = normalizedUrl
        detailMode = false
        clearSeason()
        urlInspector.inspect(normalizedUrl)
    }

    // 解析完成后的默认选择：
    //   简单视图 —— 落一条"这条视频真的有的"档位（见 pickInitialTier）
    //   详情视图 —— 视频/音频各预选优先级最高的一条（点过别的就听用户的）
    function autoPickDefaults() {
        if (!detailMode) {
            if (!live) {
                // 这一集之前挑过画质（比如返回列表又点进来）→ 恢复那一档，
                // 别用设置里的默认档顶掉用户的记忆
                const remembered = specFor(urlInspector.probingUrl)
                urlInspector.selectTier(remembered !== "" ? remembered : pickInitialTier())
            }
            return
        }
        if (urlInspector.pickedVideoId === "" && urlInspector.videoFormats.length > 0)
            urlInspector.selectFormat(urlInspector.videoFormats[0].id, 0)
        if (urlInspector.pickedAudioId === "" && urlInspector.audioFormats.length > 0)
            urlInspector.selectFormat(urlInspector.audioFormats[0].id, 1)
    }

    // 根据设置默认画质选择档位
    function pickInitialTier() {
        const want = parseInt(appSettings.defaultQuality)
        const list = urlInspector.qualities
        if (!(want > 0) || list.length === 0)
            return "0"

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
        const target = best > 0 ? best : smallest
        for (let i = 0; i < list.length; ++i) {
            if (list[i].height === target)
                return list[i].spec
        }
        return "0"
    }

    // 剧集列表的一行（给 FormatList 用，键必须是它认识的 { id, title, detail }）:
    //   id 用 "ep:" + 下标
    //   title 是"第 N 集 · 名字";detail 放时长
    readonly property var episodeRows: {
        var rows = []
        if (!isPlaylist)
            return rows
        for (var i = 0; i < seasonEpisodes.length; ++i) {
            var episode = seasonEpisodes[i]
            rows.push({
                id: "ep:" + i,
                title: episode.title,
                detail: episode.durationMs > 0 ? Style.formatTime(episode.durationMs) : "",
                hdr: false
            })
        }
        return rows
    }

    function clearSeason() {
        pickedEpisode = -1
        seasonEpisodes = []
        seasonTitle = ""
        episodeSpecs = ({})
    }

    // 记录某一集选择的画质（spec）,返回整季列表再点进来就能恢复
    function setEpisodeSpec(url, spec) {
        if (url === "" || spec === "")
            return
        const next = Object.assign({}, episodeSpecs)
        next[url] = spec
        episodeSpecs = next
    }

    // 某一集记过的 spec
    function specFor(url) {
        return url !== "" && episodeSpecs[url] !== undefined ? episodeSpecs[url] : ""
    }

    // 判断是否是当前正在探测的剧集
    function isCurrentEpisode(url) {
        return url !== "" && url === urlInspector.probingUrl
    }

    function pickEpisodeByRow(rowId) {
        const index = parseInt(String(rowId).slice(3))   // 去掉 "ep:"
        if (!(index >= 0) || index >= seasonEpisodes.length)
            return
        pickedEpisode = index
        const url = seasonEpisodes[index].url
        const remembered = specFor(url)
        urlInspector.pickEpisode(index)
        // 记过画质的集：这一趟解析一结束就用回那一档(槽位在结果出来前先占上，
        // autoPickDefaults 会按记忆恢复)。没记过的：清槽，让它走默认档
        if (remembered !== "") {
            episodeSpecs = Object.assign({}, episodeSpecs, { [url]: remembered })
        } else {
            const next = Object.assign({}, episodeSpecs)
            delete next[url]
            episodeSpecs = next
        }
    }

    // 从"某一集的画质页面"返回整季列表。整季的解析结果一直在探测器手里
    // (见 UrlInspector::m_playlistJson),这里不重跑 yt-dlp,瞬间就回来。
    // 输入框换回整季的地址 —— 不然用户看到的是某一集的地址，却显示着整季列表
    function backToEpisodes() {
        pickedEpisode = -1
        urlInspector.backToPlaylist()
        urlField.text = inspectedUrl
    }

    // 把"这一集"按当前选中的档位加进播放列表。
    // itemId 用**剧集自己的 itemId**(见 C++ 的 Episodes::itemId):
    // 同一集在各端拿到同一个 id,服务端查重才认得出"你们加的是同一条";
    // 不加的话各端随机生成，列表里会出两套"第 1 集"
    function addEpisode(episode) {
        const url = episode.url
        // 档位来源，按优先级：
        //   ① 这一条就是刚解析出来的 → 用当前的选择（用户刚挑的/恢复的记忆都在里面）
        //   ② 这一集记过画质（之前挑过）→ 用它
        //   ③ 都没有 → "最高画质"("0")
        const spec = isCurrentEpisode(url) && canSubmit
                     ? urlInspector.buildSpec(appSettings.hdrAllowed)
                     : (specFor(url) !== "" ? specFor(url) : "0")
        roomSession.addUrlWithId(episode.itemId, url, episode.title,
                                 false, episode.durationMs, spec)
    }

    // 整季加进去。每条用自己的 url/itemId;档位见 addEpisode ——
    // 个别集没有这一档时播放端会退到它的最高画质，不会失败
    function submitAllEpisodes() {
        if (!isPlaylist)
            return
        for (let i = 0; i < seasonEpisodes.length; ++i)
            addEpisode(seasonEpisodes[i])
        root.close()
    }

    function submit() {
        // 剧集：加的是**点定的那一集**(不是用户粘贴的整季地址)
        if (episodePicked) {
            addEpisode(pickedEpisodeData)
            // 不关窗：一季通常要连着加几集，关掉就得重新粘链接、重新解析
            return
        }
        if (!canSubmit)
            return
        // 参数必须**全部传满**:Q_INVOKABLE 的默认参数在 QML 里不存在，
        // 少传一个会直接报 "Insufficient arguments"
        roomSession.addUrl(normalizedUrl, urlInspector.title, urlInspector.live,
                           urlInspector.durationMs,
                           urlInspector.buildSpec(appSettings.hdrAllowed))
        root.close()
    }

    onOpened: {
        page = 0
        urlField.text = ""
        urlField.placeholderText = "粘贴完整视频链接"
        urlField.forceActiveFocus()
        inspectedUrl = ""
        sourcePlatform = "generic"
        probedInput = ""
        normalizedUrl = ""
        normalizeError = ""
        detailMode = false
        clearSeason()
    }
    onClosed: urlInspector.cancel()

    Connections {
        target: urlInspector
        function onInspected() {
            root.detailMode = false
            // 解析出来是整季 → 存一份本地快照（后面所有判断/显示都读它）
            if (urlInspector.playlistView) {
                root.seasonEpisodes = urlInspector.episodes
                root.seasonTitle = urlInspector.playlistTitle
            }
            root.autoPickDefaults()
        }
    }

    background: Rectangle {
        radius: Style.radius
        color: Style.card
        border.width: 1
        border.color: Style.border
    }

    contentItem: Column {
        id: content
        spacing: 14

        // ── 头部：标题 + 关闭 ──
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

        // ══════ 第 0 页：选来源 ══════
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

            // 选项卡，以后可以加入更多来源
            Rectangle {
                id: bilibiliCard
                width: parent.width
                height: 58
                radius: Style.radius
                color: bilibiliHover.containsMouse ? Style.fieldHover : Style.fieldBg
                border.width: 1
                border.color: bilibiliHover.containsMouse ? Style.accent : Style.border

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
                            text: "Bilibili"
                            font.pixelSize: 13
                            color: Style.textPrimary
                        }

                        Text {
                            text: "BV号 / av号 / ss号 / ep号 / 直播间号，也可粘贴链接"
                            font.pixelSize: 11
                            color: Style.textFaint
                        }
                    }
                }

                MouseArea {
                    id: bilibiliHover
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.sourcePlatform = "bilibili"
                        urlField.placeholderText = "BV号 / av号 / ss号 / ep号 / 直播间号 / 链接"
                        root.page = 1
                        urlField.forceActiveFocus()
                    }
                }
            }

            Text {
                width: parent.width
                visible: root.sourcePlatform === "bilibili"
                text: "纯数字按直播间号处理；番剧请加 ss/ep 前缀。"
                font.pixelSize: 11
                color: Style.textFaint
            }

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
                            text: "粘贴完整视频链接，可选画质"
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
                        root.sourcePlatform = "generic"
                        urlField.placeholderText = "粘贴完整视频链接"
                        root.page = 1
                        urlField.forceActiveFocus()
                    }
                }
            }
        }

        // ══════ 第 1 页：粘链接 + 挑画质 ══════
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
                        urlInspector.cancel()
                    }
                }
            }

            Text {
                width: parent.width
                visible: !toolchain.ready
                text: "缺少外部工具：" + toolchain.statusText
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
                    enabled: root.inputLooksUsable && !root.probing
                    onClicked: root.startProbe()
                }
            }

            // 失败原因显示（平台编号规范化失败 / yt-dlp 解析失败共用这块）
            Text {
                width: parent.width
                visible: root.normalizeError !== "" || urlInspector.stage === "failed"
                text: root.normalizeError !== "" ? root.normalizeError
                                                 : urlInspector.errorText
                wrapMode: Text.Wrap
                font.pixelSize: 11
                color: Style.danger
            }

            // ── 剧集（多集链接）──
            // 点某一集 → 重新解析那一集 → 下面的画质档才出现。
            // 画质档针对**那一集**归纳（各集 formats 可能不同）,所以顺序是
            // "先点集、后挑画质",不能反过来
            Column {
                width: parent.width
                spacing: 6

                visible: root.isPlaylist

                Text {
                    width: parent.width
                    text: root.seasonTitle !== ""
                          ? root.seasonTitle + "(共 " + root.seasonEpisodes.length + " 集)"
                          : "共 " + root.seasonEpisodes.length + " 集"
                    font.pixelSize: 14
                    color: Style.textPrimary
                    wrapMode: Text.Wrap
                }

                // 集数可能很多：给个最大高度自己滚
                Flickable {
                    width: parent.width
                    height: Math.min(episodeColumn.implicitHeight, 260)
                    contentWidth: width
                    contentHeight: episodeColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    Column {
                        id: episodeColumn
                        width: parent.width
                        spacing: 6

                        FormatList {
                            width: parent.width
                            options: root.episodeRows
                            pickedId: root.episodePicked
                                      ? "ep:" + root.pickedEpisode : ""
                            onPicked: (id) => root.pickEpisodeByRow(id)
                        }

                        Text {
                            visible: urlInspector.pickingEpisode
                            text: "正在解析这一集…"
                            font.pixelSize: 12
                            color: Style.textFaint
                        }
                    }
                }

                PrimaryButton {
                    width: parent.width
                    text: "加入整季（" + root.seasonEpisodes.length + " 集）"
                    onClicked: root.submitAllEpisodes()
                }
            }

            // ── 点定某一集之后：返回剧集列表 ──
            // **独立一行**而不是塞进上面那块：点了一集之后剧集列表整块是隐藏的，
            // 放里面就等于没有返回入口（第一版就是这么错的）
            GhostButton {
                width: parent.width
                visible: root.episodePicked
                text: "‹ 返回全部剧集"
                onClicked: root.backToEpisodes()
            }

            // 解析结果：标题 + 时长/直播 + 上传者（纯文字，不做缩略图）
            // (多集链接不显示：那上面显示的是整季的名字，不是某一条的)
            Column {
                width: parent.width
                spacing: 4

                visible: root.inspectedOk && !root.isPlaylist

                Text {
                    width: parent.width
                    text: urlInspector.title
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
                        if (urlInspector.live)
                            parts.push("直播")
                        else if (urlInspector.durationMs > 0)
                            parts.push(Style.formatTime(urlInspector.durationMs))
                        if (urlInspector.uploader !== "")
                            parts.push(urlInspector.uploader)
                        return parts.join(" · ")
                    }
                }
            }

            // ── 画质：简单视图 ──
            Column {
                width: parent.width
                spacing: 4

                visible: root.inspectedOk && !root.detailMode && !root.isPlaylist

                RowLayout {
                    width: parent.width

                    Text {
                        text: "画质"
                        font.pixelSize: 12
                        color: Style.textSecondary
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: "详情 ▾"
                        font.pixelSize: 12
                        color: detailHover.containsMouse ? Style.accent : Style.textFaint

                        MouseArea {
                            id: detailHover
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.detailMode = true
                                root.autoPickDefaults()
                            }
                        }
                    }
                }

                // 选项里每一项的结构见 AddVideoDialog 顶部说明。
                //
                // **高度显式算**(行高 32 + 行距 2,见 OptionList):别指望
                // `implicitHeight: tierList.implicitHeight` 那种隐式高度的层层传递 ——
                // 传递链断在哪一环不好查，而断掉的表现很迷惑：行看得见、点不动
                // (Item 高度 0 → MouseArea 命中区域 0)。行高就写在 OptionList 里，
                // 改那边记得回来改这里。
                Item {
                    id: tierPicker
                    width: parent.width
                    height: root.qualityOptions.length * 34

                    OptionList {
                        id: tierList
                        anchors.left: parent.left
                        anchors.right: parent.right
                        options: root.qualityOptions
                        selectedValue: urlInspector.tierSpec
                        onPicked: (value) => {
                            root.selectTier(value)
                            // 按集记住这次的选择（"加入整季"/返回再进都靠它）
                            if (root.episodePicked && root.pickedEpisodeData !== null)
                                root.setEpisodeSpec(root.pickedEpisodeData.url,
                                                    urlInspector.buildSpec(appSettings.hdrAllowed))
                        }
                    }
                }
            }

            // ── 画质：详情视图 ──
            Column {
                width: parent.width
                spacing: 8

                visible: root.inspectedOk && root.detailMode && !root.isPlaylist

                RowLayout {
                    width: parent.width

                    Text {
                        text: "逐条选择"
                        font.pixelSize: 12
                        color: Style.textSecondary
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        text: "返回简略 ‹"
                        font.pixelSize: 12
                        color: backHover.containsMouse ? Style.accent : Style.textFaint

                        MouseArea {
                            id: backHover
                            anchors.fill: parent
                            anchors.margins: -6
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.detailMode = false
                                // 手选的那对 id 不清：回到简单视图只是想换个看法，
                                // 用户再点一次档位才会真的把它替换掉
                            }
                        }
                    }
                }

                Flickable {
                    width: parent.width
                    height: Math.min(contentHeight, 320)
                    contentWidth: width
                    contentHeight: streamColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    Column {
                        id: streamColumn
                        width: parent.width
                        spacing: 8

                        Text {
                            visible: urlInspector.videoFormats.length > 0
                            text: urlInspector.live ? "直播流（选一条）" : "视频流（选一条）"
                            font.pixelSize: 11
                            color: Style.textFaint
                        }

                        FormatList {
                            width: parent.width
                            options: urlInspector.videoFormats
                            pickedId: urlInspector.pickedVideoId
                            onPicked: (id) => urlInspector.selectFormat(id, 0)
                        }

                        Text {
                            visible: urlInspector.audioFormats.length > 0
                            text: "音频流（不选则默认最优规格）"
                            font.pixelSize: 11
                            color: Style.textFaint
                        }

                        FormatList {
                            width: parent.width
                            options: urlInspector.audioFormats
                            pickedId: urlInspector.pickedAudioId
                            onPicked: (id) => urlInspector.selectFormat(id, 1)
                        }

                        Text {
                            visible: urlInspector.combinedFormats.length > 0
                            text: "合并格式（音视频一体，无需另选音频）"
                            font.pixelSize: 11
                            color: Style.textFaint
                        }

                        FormatList {
                            width: parent.width
                            options: urlInspector.combinedFormats
                            pickedId: urlInspector.pickedAudioId === urlInspector.pickedVideoId
                                      ? urlInspector.pickedVideoId : ""
                            onPicked: (id) => urlInspector.selectFormat(id, 2)
                        }
                    }
                }

                Text {
                    width: parent.width
                    visible: {
                        const picked = urlInspector.pickedVideoId
                        if (picked === "")
                            return false
                        const list = urlInspector.videoFormats.concat(urlInspector.combinedFormats)
                        for (let i = 0; i < list.length; ++i) {
                            if (list[i].id === picked)
                                return list[i].hdr === true
                        }
                        return false
                    }
                    text: appSettings.hdrAllowed
                          ? "该流为 HDR/杜比视界；设置中已允许 HDR，播放时高光部分可能过曝"
                          : "该流为 HDR/杜比视界；设置中已关闭 HDR，添加后将无法下载"
                    wrapMode: Text.Wrap
                    font.pixelSize: 11
                    color: Style.danger
                }
            }

            // 点定某一集之后，这里的按钮就是把**那一集**加进去
            // (多集链接未点定任何一集时，看上面的"加入整季"按钮)
            PrimaryButton {
                width: parent.width
                visible: !root.isPlaylist || root.episodePicked
                text: root.episodePicked ? "加入这一集" : "加入播放列表"
                enabled: root.episodePicked || root.canSubmit
                onClicked: root.submit()
            }
        }
    }
}
