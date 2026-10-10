import QtQuick

// 弹幕层:消息来了就从右往左飘过画面。
//
// 先说清楚它"简"在哪(后面做真了要改这两点):
//   · 不跟视频时间轴绑定 —— 收到就飘。而不是"发在 12:34 的那条,
//     等播到 12:34 才出现"。后者要消息带视频时间戳 + 重播时按时间轴重放。
//   · 轨道轮转分配,不做碰撞检测 —— 同一条轨道上前后两条可能追上。
//
// 弹幕和评论区用的是同一份消息,来源在 RoomPage。
Item {
    id: root

    property bool running: true
    property int laneCount: 5

    // 下一条弹幕放哪条轨道。轮着来,简单且不会全挤在一条线上
    property int _lane: 0

    clip: true

    function push(message) {
        if (!running || message === "" || width <= 1 || height <= 1)
            return

        const lane = _lane
        _lane = (_lane + 1) % laneCount
        const laneHeight = height / laneCount

        sprite.createObject(root, {
            "text": message,
            "y": lane * laneHeight + 4,
            "startX": root.width
        })
    }

    Component {
        id: sprite

        Text {
            id: dm

            property real startX: 0

            color: "#FFFFFF"
            font.pixelSize: 16
            // 白字描黑边:视频画面是亮是暗都看得清,比阴影保险
            style: Text.Outline
            styleColor: "#99000000"

            x: startX

            NumberAnimation on x {
                // 终点是"整条完全移出左边",所以终点跟自身宽度有关
                to: -dm.width
                // 时长按距离算,让所有弹幕速度一致(约 110 像素/秒)
                duration: Math.max(4000, (dm.startX + dm.width) / 0.11)
                running: true
                onFinished: dm.destroy()
            }
        }
    }
}
