import QtQuick

// 弹幕层。三个来源共用同一张轨道表：
//   · 聊天弹幕 push(...)：房间消息，气泡，暂停时也继续飘；
//   · 点播弹幕：C++ 解析 B 站 XML，按 playbackSync.position 由泵逐批取出，
//     在正确时间点出现；暂停时冻结、seek 时清屏；
//   · 直播弹幕：C++ 通过 WebSocket 实时推送，pushLive(...) 收到即显示，
//     不受暂停/seek 影响（直播没有共同时间轴）。
//
// 轨道分配用屏幕上 sprite 的实际 x/width 判断，不靠时间估算：
// 同一条轨道上，前一条的尾巴还没让出右边就不放新的；滚动/顶部/底部共用轨道，
// 避免固定弹幕和滚动弹幕叠在同一行。
//
// **暂停不能用 NumberAnimation.running 绑定**：running 从 false 变 true 会从头
// 重播动画，表现是暂停再播时弹幕跳回起点。必须用 pause()/resume()。
Item {
    id: root

    // 由 VideoPlayer 绑定 danmakuVisible：关闭时清掉时间轴弹幕
    property bool running: true

    readonly property int _maxSprites: 200
    readonly property int _laneSlots: 16

    // 字号/轨道随播放器高度缩放。XML 的 25 号按 720 高设计。
    // “随屏幕缩放”关闭时用固定的 720p 基准。
    readonly property real _screenScale:
        appSettings.danmakuScaleWithScreen
            ? Math.max(0.6, Math.min(2.0, height / 720))
            : 1.0
    readonly property real _fontPercent: appSettings.danmakuFontSize / 100
    readonly property real _baseFont: _pixelSize(25)
    readonly property real _laneHeight: Math.max(20, _baseFont * 1.8)
    // 显示区域：弹幕只占播放器顶部的这个比例
    readonly property real _areaHeight:
        Math.max(20, height * Math.max(10, appSettings.danmakuArea) / 100)
    readonly property int _laneCount:
        Math.max(1, Math.min(_laneSlots, Math.floor(_areaHeight / _laneHeight)))
    readonly property real _gap: Math.max(12, _baseFont * 0.8)
    // 速度五档：0.5x / 0.75x / 1.0x / 1.25x / 1.5x
    readonly property real _speedFactor: {
        const levels = [0.5, 0.75, 1.0, 1.25, 1.5]
        return levels[Math.max(0, Math.min(4, appSettings.danmakuSpeed))]
    }

    opacity: Math.max(0.1, Math.min(1.0, appSettings.danmakuOpacity / 100))

    function _pixelSize(baseSize) {
        return Math.max(8, Math.min(64,
            Math.round(baseSize * root._screenScale * root._fontPercent)))
    }

    // 每条轨道上的活动 sprite（滚动 + 固定混在一起）
    property var _lanes: []
    property var _allSprites: []
    property var _timelineSprites: []
    property int _nextLane: 0
    property int _chatLane: 0
    property int _liveLane: 0

    clip: true

    Component.onCompleted: {
        for (let i = 0; i < _laneSlots; ++i)
            _lanes.push([])
    }

    onRunningChanged: {
        if (!running) {
            clearTimeline()
            clearLive()
        } else {
            // 关闭期间不再补发，从当前进度继续
            danmakuController.seekTo(playbackSync.position)
        }
    }

    // ============================
    // 聊天弹幕（房间成员 / 自己）
    // ============================
    // 与解析出的视频弹幕区分开：房间消息走气泡 + 发送者，自己的气泡是主色蓝，
    // 别人的是深色底、昵称用主色。解析出的弹幕仍是纯文字。
    function push(message, sender, isSelf) {
        if (!running || message === "" || width <= 1 || height <= 1)
            return
        if (_allSprites.length >= _maxSprites)
            return

        let lane = _pickScrollLane(1)
        if (lane < 0) {
            // 时间轴弹幕把轨道占满时聊天仍要能看到：退回轮转轨道，允许重叠
            lane = _chatLane
            _chatLane = (_chatLane + 1) % _laneCount
        }
        _spawnChat(message, sender, isSelf, lane)
    }

    function _spawnChat(text, sender, isSelf, lane) {
        const name = isSelf === true
                     ? "我"
                     : (sender && sender !== "" ? String(sender) : "房间")
        const sprite = scrollSprite.createObject(root, {
            "dmBubble": true,
            "dmText": text,
            "dmSender": name,
            "dmSelf": isSelf === true,
            "dmPixelSize": _pixelSize(16),
            "dmLane": lane,
            "dmDirection": 1,
            "dmSpeed": 110 * _speedFactor * _rate(),
            "dmTimeline": false,
            "dmMode": 1
        })
        if (!sprite)
            return
        _register(sprite, lane)
        sprite.launch()
    }

    // ============================
    // 时间轴弹幕
    // ============================
    Timer {
        interval: 50
        repeat: true
        running: root.running && danmakuController.available
                 && playbackSync.playing
        onTriggered: root._pump()
    }

    function _pump() {
        const position = playbackSync.position
        // takeUntil 内部检测 seek/纠偏；跳变时会发 jumped，先清屏再返回新位置上的条目
        const items = danmakuController.takeUntil(position)
        for (let i = 0; i < items.length; ++i)
            _spawnTimeline(items[i])
        _cleanupFixed(position)
    }

    function _spawnTimeline(item) {
        if (!running || width <= 1 || height <= 1)
            return

        const text = String(item.text || "")
        if (text === "")
            return

        const rawMode = Number(item.mode)
        const mode = Number.isFinite(rawMode) && rawMode > 0 ? rawMode : 1
        const rawSize = Number(item.size)
        const pixelSize = _pixelSize(rawSize > 0 ? rawSize : 25)
        const color = String(item.color || "#FFFFFF")

        if (_allSprites.length >= _maxSprites)
            return

        if (mode === 4 || mode === 5) {
            const lane = _pickFixedLane(mode)
            if (lane < 0)
                return
            const sprite = fixedSprite.createObject(root, {
                "text": text,
                "color": color,
                "dmOutline": _outlineFor(color),
                "dmPixelSize": pixelSize,
                "dmLane": lane,
                "dmMode": mode,
                "dmExpireAt": playbackSync.position + 4000
            })
            if (sprite)
                _register(sprite, lane)
            return
        }

        const direction = mode === 6 ? -1 : 1
        const lane = _pickScrollLane(direction)
        if (lane < 0)
            return // 轨道满了：时间轴弹幕丢弃（B 站行为）

        _spawnTimedScroll(text, color, pixelSize, lane, direction, mode)
    }

    // ============================
    // 轨道
    // ============================
    function _laneFree(lane) {
        return !_lanes[lane] || _lanes[lane].length === 0
    }

    function _laneFreeForScroll(lane, direction) {
        const sprites = _lanes[lane]
        if (!sprites || sprites.length === 0)
            return true

        for (let i = 0; i < sprites.length; ++i) {
            const sprite = sprites[i]
            if (!sprite)
                continue
            // 固定弹幕占整条轨道；反向弹幕也不共用
            if (sprite.dmMode === 4 || sprite.dmMode === 5)
                return false
            if (sprite.dmDirection !== direction)
                return false
        }

        const last = sprites[sprites.length - 1]
        if (!last)
            return true
        if (direction > 0)
            return last.x + last.width + _gap <= root.width
        return last.x >= _gap
    }

    function _pickScrollLane(direction) {
        for (let i = 0; i < _laneCount; ++i) {
            const lane = (_nextLane + i) % _laneCount
            if (_laneFreeForScroll(lane, direction)) {
                _nextLane = (lane + 1) % _laneCount
                return lane
            }
        }
        return -1
    }

    function _pickFixedLane(mode) {
        // 顶部从第 0 条往下，底部从最后一条往上
        if (mode === 5) {
            for (let i = 0; i < _laneCount; ++i)
                if (_laneFree(i))
                    return i
        } else {
            for (let i = _laneCount - 1; i >= 0; --i)
                if (_laneFree(i))
                    return i
        }
        return -1
    }

    function _register(sprite, lane) {
        if (!_lanes[lane])
            _lanes[lane] = []
        _lanes[lane].push(sprite)
        _allSprites.push(sprite)
        if (sprite.dmTimeline)
            _timelineSprites.push(sprite)
    }

    function removeSprite(sprite) {
        const laneSprites = _lanes[sprite.dmLane]
        if (laneSprites) {
            const index = laneSprites.indexOf(sprite)
            if (index >= 0)
                laneSprites.splice(index, 1)
        }

        let index = _allSprites.indexOf(sprite)
        if (index >= 0)
            _allSprites.splice(index, 1)
        index = _timelineSprites.indexOf(sprite)
        if (index >= 0)
            _timelineSprites.splice(index, 1)
    }

    // ============================
    // sprite 生命周期
    // ============================
    function _spawnTimedScroll(text, color, pixelSize, lane, direction, mode) {
        const sprite = scrollSprite.createObject(root, {
            "dmBubble": false,
            "dmText": text,
            "dmColor": color,
            "dmOutline": _outlineFor(color),
            "dmPixelSize": pixelSize,
            "dmLane": lane,
            "dmDirection": direction,
            "dmSpeed": 110 * _speedFactor * _rate(),
            "dmTimeline": true,
            "dmMode": mode
        })
        if (!sprite)
            return
        _register(sprite, lane)
        sprite.launch()
    }

    function clearTimeline() {
        const copy = _timelineSprites.slice()
        for (let i = 0; i < copy.length; ++i) {
            if (copy[i])
                copy[i].destroy()
        }
        _timelineSprites = []
    }

    // ============================
    // 直播弹幕（WebSocket 实时推送）
    // ============================
    // 和点播弹幕一样是纯文字；但 dmTimeline=false，不受暂停/seek 影响，
    // 也不进入 _timelineSprites，所以 clearTimeline 不会误删它。
    function pushLive(item) {
        if (!running || width <= 1 || height <= 1)
            return
        if (_allSprites.length >= _maxSprites)
            return

        const text = String(item.text || "")
        if (text === "")
            return

        const rawMode = Number(item.mode)
        const mode = Number.isFinite(rawMode) && rawMode > 0 ? rawMode : 1
        const rawSize = Number(item.size)
        const pixelSize = _pixelSize(rawSize > 0 ? rawSize : 25)
        const color = String(item.color || "#FFFFFF")
        const direction = mode === 6 ? -1 : 1

        const lane = _pickScrollLane(direction)
        let targetLane = lane
        if (targetLane < 0) {
            // 直播优先保证"每条都出现"：所有轨道都占满时退回轮转轨道，
            // 允许重叠，而不是像点播那样直接丢弃。
            targetLane = _liveLane
            _liveLane = (_liveLane + 1) % _laneCount
        }

        const sprite = scrollSprite.createObject(root, {
            "dmBubble": false,
            "dmLive": true,
            "dmText": text,
            "dmColor": color,
            "dmOutline": _outlineFor(color),
            "dmPixelSize": pixelSize,
            "dmLane": targetLane,
            "dmDirection": direction,
            "dmSpeed": 110 * _speedFactor,
            "dmTimeline": false,
            "dmMode": mode
        })
        if (!sprite)
            return
        _register(sprite, targetLane)
        sprite.launch()
    }

    function clearLive() {
        const copy = _allSprites.slice()
        for (let i = 0; i < copy.length; ++i) {
            if (copy[i] && copy[i].dmLive)
                copy[i].destroy()
        }
        _liveLane = 0
    }

    // 设置变化时连聊天弹幕一起清掉：字号/轨道/速度都已经变了，
    // 让旧 sprite 按旧参数飞完反而会与新区间重叠。
    function clearAll() {
        const copy = _allSprites.slice()
        for (let i = 0; i < copy.length; ++i) {
            if (copy[i])
                copy[i].destroy()
        }
        _allSprites = []
        _timelineSprites = []
        for (let i = 0; i < _lanes.length; ++i)
            _lanes[i] = []
        _nextLane = 0
        _chatLane = 0
        _liveLane = 0
    }

    function _cleanupFixed(position) {
        const copy = _timelineSprites.slice()
        for (let i = 0; i < copy.length; ++i) {
            const sprite = copy[i]
            if (sprite && !sprite.dmAnimated
                && (sprite.dmMode === 4 || sprite.dmMode === 5)
                && position >= sprite.dmExpireAt) {
                sprite.destroy()
            }
        }
    }

    // ============================
    // 工具
    // ============================
    function _rate() {
        const rate = Number(playbackSync.playbackRate)
        return rate > 0 ? rate : 1
    }

    function _outlineFor(color) {
        let hex = String(color || "#FFFFFF").replace("#", "")
        if (hex.length !== 6)
            hex = "FFFFFF"
        const r = parseInt(hex.substring(0, 2), 16) / 255
        const g = parseInt(hex.substring(2, 4), 16) / 255
        const b = parseInt(hex.substring(4, 6), 16) / 255
        const luminance = 0.299 * r + 0.587 * g + 0.114 * b
        // 亮字黑边，暗字白边：深色弹幕在暗画面上也能看清
        return luminance > 0.55 ? "#B0000000" : "#B0FFFFFF"
    }

    // ============================
    // 信号接线
    // ============================

    Connections {
        target: danmakuController

        function onSourceChanged() {
            root.clearTimeline()
            root.clearLive()
        }

        function onJumped() {
            root.clearTimeline()
        }

        function onAvailableChanged() {
            if (!danmakuController.available)
                root.clearTimeline()
        }

        function onLiveDanmakuBatch(items) {
            for (let i = 0; i < items.length; ++i)
                root.pushLive(items[i])
        }

        function onLiveStopped() {
            root.clearLive()
        }
    }

    Connections {
        target: playbackSync

        function onPlayingChanged() {
            const copy = root._timelineSprites.slice()
            for (let i = 0; i < copy.length; ++i) {
                const sprite = copy[i]
                if (!sprite || !sprite.dmAnimated)
                    continue
                if (playbackSync.playing)
                    sprite.resumeTimeline()
                else
                    sprite.pauseTimeline()
            }
        }
    }

    // 这些设置影响轨道高度/字号/速度，已经飞在屏幕上的弹幕无法平滑改，
    // 直接清屏，让新设置从下一批开始生效。
    // 不透明度不用清：根 Item 的 opacity 绑定会自动更新到所有 sprite。
    Connections {
        target: appSettings

        function onDanmakuAreaChanged() {
            root.clearAll()
        }

        function onDanmakuFontSizeChanged() {
            root.clearAll()
        }

        function onDanmakuSpeedChanged() {
            root.clearAll()
        }

        function onDanmakuScaleWithScreenChanged() {
            root.clearAll()
        }
    }

    // ============================
    // sprite
    // ============================

    // 滚动/逆向弹幕：x 由 NumberAnimation 驱动，暂停用 pause()/resume()。
    // 同一个 Item 承载两种形态：
    //   · dmBubble=false：解析出的视频弹幕，纯文字 + 描边；
    //   · dmBubble=true ：房间聊天，圆角气泡 + 发送者（自己的是主色蓝底）。
    Component {
        id: scrollSprite

        Item {
            id: dm

            property bool dmBubble: false
            property bool dmLive: false
            property string dmText: ""
            property string dmSender: ""
            property bool dmSelf: false
            property color dmColor: "#FFFFFF"
            property color dmOutline: "#B0000000"
            property bool dmTimeline: false
            property bool dmAnimated: true
            property int dmLane: 0
            property int dmDirection: 1   // 1: 右→左，-1: 左→右
            property int dmMode: 1
            property int dmPixelSize: 16
            property real dmSpeed: 110    // px/s
            property bool dmPaused: false

            width: dmBubble ? bubble.width : label.width
            height: dmBubble ? bubble.height : label.height
            x: 0
            y: dm.dmLane * root._laneHeight
               + Math.max(0, (root._laneHeight - height) / 2)

            // 视频弹幕：纯文字，白/彩色字 + 描边
            Text {
                id: label
                visible: !dm.dmBubble
                text: dm.dmText
                color: dm.dmColor
                textFormat: Text.PlainText
                style: Text.Outline
                styleColor: dm.dmOutline
                font.pixelSize: dm.dmPixelSize
            }

            // 房间聊天：小圆角气泡。自己的是主色底，别人的是深色底。
            Rectangle {
                id: bubble
                visible: dm.dmBubble
                width: row.width + 16
                height: row.height + 8
                radius: 4
                color: dm.dmSelf ? "#E600AEEC" : "#CC000000"

                Row {
                    id: row
                    anchors.centerIn: parent
                    spacing: 0

                    Text {
                        text: dm.dmSender + "："
                        color: dm.dmSelf ? "#FFFFFF" : "#00AEEC"
                        textFormat: Text.PlainText
                        font.pixelSize: dm.dmPixelSize
                    }

                    Text {
                        text: dm.dmText
                        color: "#FFFFFF"
                        textFormat: Text.PlainText
                        font.pixelSize: dm.dmPixelSize
                    }
                }
            }

            NumberAnimation on x {
                id: anim
                running: false
                onFinished: dm.destroy()
            }

            function launch() {
                const from = dm.dmDirection > 0 ? root.width : -dm.width
                const to = dm.dmDirection > 0 ? -dm.width : root.width
                dm.x = from
                anim.from = from
                anim.to = to
                anim.duration = Math.max(
                    400, Math.abs(to - from) / Math.max(1, dm.dmSpeed) * 1000)
                anim.start()
            }

            // 聊天弹幕（dmTimeline=false）永远不暂停
            function pauseTimeline() {
                if (!dm.dmTimeline || !dm.dmAnimated || dm.dmPaused)
                    return
                dm.dmPaused = true
                anim.pause()
            }

            function resumeTimeline() {
                if (!dm.dmTimeline || !dm.dmAnimated || !dm.dmPaused)
                    return
                dm.dmPaused = false
                anim.resume()
            }

            Component.onDestruction: root.removeSprite(dm)
        }
    }

    // 顶部/底部弹幕：固定 4 秒（媒体时间），由 _cleanupFixed 销毁
    Component {
        id: fixedSprite

        Text {
            id: dm

            property bool dmTimeline: true
            property bool dmAnimated: false
            property int dmLane: 0
            property int dmMode: 5
            property real dmExpireAt: 0
            property int dmPixelSize: 25
            property color dmOutline: "#B0000000"

            textFormat: Text.PlainText
            style: Text.Outline
            styleColor: dm.dmOutline
            font.pixelSize: dm.dmPixelSize
            color: "#FFFFFF"
            x: (root.width - width) / 2
            y: dm.dmLane * root._laneHeight
               + Math.max(0, (root._laneHeight - height) / 2)

            Component.onDestruction: root.removeSprite(dm)
        }
    }
}
