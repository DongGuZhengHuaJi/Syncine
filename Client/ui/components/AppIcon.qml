import QtQuick

// 线性图标,全部用 Canvas 画。
//
// 为什么不上 SVG:项目没链接 Qt6::Svg,QML 的 Image 加载不了 .svg;
// 也不引图标字体(要额外打包字体文件)。Canvas 是零依赖的,
// 而且现有代码(播放/静音图标)本来就是这么画的。
//
// 所有图标都按 24x24 的坐标系画,再按实际尺寸缩放 —— 这样写路径时
// 脑子里有个固定的画布,不用为每个尺寸重算坐标。
Item {
    id: root

    property string name: ""
    // 描边粗细也是 24 坐标系里的值
    property color color: "#FFFFFF"
    property real strokeWidth: 1.7

    implicitWidth: 18
    implicitHeight: 18

    onNameChanged: canvas.requestPaint()
    onColorChanged: canvas.requestPaint()
    onStrokeWidthChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true

        Component.onCompleted: requestPaint()

        onPaint: {
            const ctx = getContext("2d")

            // 先按"item 坐标"清屏 —— 此时还没施加缩放,清的区域才是整个画布
            ctx.clearRect(0, 0, width, height)

            const s = Math.min(width, height) / 24
            ctx.save()
            ctx.scale(s, s)
            ctx.lineWidth = root.strokeWidth
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.strokeStyle = root.color
            ctx.fillStyle = root.color

            const line = (x1, y1, x2, y2) => {
                ctx.beginPath()
                ctx.moveTo(x1, y1)
                ctx.lineTo(x2, y2)
                ctx.stroke()
            }
            const poly = (pts, fill) => {
                ctx.beginPath()
                ctx.moveTo(pts[0], pts[1])
                for (let i = 2; i < pts.length; i += 2)
                    ctx.lineTo(pts[i], pts[i + 1])
                if (fill)
                    ctx.fill()
                else
                    ctx.stroke()
            }
            const box = (x, y, w, h) => {
                ctx.beginPath()
                ctx.rect(x, y, w, h)
                ctx.stroke()
            }
            const circle = (cx, cy, r, a0, a1) => {
                ctx.beginPath()
                ctx.arc(cx, cy, r, a0 === undefined ? 0 : a0,
                        a1 === undefined ? Math.PI * 2 : a1)
                ctx.stroke()
            }

            switch (root.name) {
            case "play":
                poly([8, 4.5, 19, 12, 8, 19.5], true)
                break

            case "pause":
                ctx.fillRect(7, 4.5, 3.8, 15)
                ctx.fillRect(13.2, 4.5, 3.8, 15)
                break

            case "back":
                line(19, 12, 5.5, 12)
                poly([11.5, 18.5, 5, 12, 11.5, 5.5])
                break

            case "folder":
                poly([3, 18.5, 3, 5.5, 9.5, 5.5, 11.5, 8.5, 21, 8.5, 21, 18.5],
                     false)
                ctx.beginPath()
                ctx.moveTo(3, 18.5)
                ctx.lineTo(21, 18.5)
                ctx.stroke()
                break

            case "folder-plus":
                poly([3, 18.5, 3, 5.5, 9.5, 5.5, 11.5, 8.5, 21, 8.5, 21, 18.5],
                     false)
                line(3, 18.5, 21, 18.5)
                line(12, 11, 12, 16)
                line(9.5, 13.5, 14.5, 13.5)
                break

            case "speaker":
                // 喇叭本体(实心)+ 右侧两道声波
                poly([11, 5, 6.5, 9, 3, 9, 3, 15, 6.5, 15, 11, 19], true)
                circle(13.5, 12, 4.2, -0.95, 0.95)
                circle(13.5, 12, 7.8, -0.8, 0.8)
                break

            case "headset":
                // 耳机:头梁 + 两侧耳罩 + 话筒杆。和"喇叭"区分度足够大
                circle(12, 12.5, 8, Math.PI, 0)
                ctx.fillRect(3, 12, 3.6, 6)
                ctx.fillRect(17.4, 12, 3.6, 6)
                ctx.beginPath()
                ctx.moveTo(19.2, 18.5)
                ctx.quadraticCurveTo(19.2, 21.5, 15, 21.5)
                ctx.stroke()
                break

            case "mic":
                // 胶囊形话筒头
                ctx.beginPath()
                ctx.arc(12, 7.6, 3, Math.PI, 0)
                ctx.lineTo(15, 11.6)
                ctx.arc(12, 11.6, 3, 0, Math.PI)
                ctx.closePath()
                ctx.fill()
                // 托架 + 支架
                circle(12, 11.2, 6.2, 0, Math.PI)
                line(12, 17.4, 12, 21)
                line(8.5, 21, 15.5, 21)
                break

            case "mic-off":
                ctx.beginPath()
                ctx.arc(12, 7.6, 3, Math.PI, 0)
                ctx.lineTo(15, 11.6)
                ctx.arc(12, 11.6, 3, 0, Math.PI)
                ctx.closePath()
                ctx.fill()
                circle(12, 11.2, 6.2, 0, Math.PI)
                line(12, 17.4, 12, 21)
                line(8.5, 21, 15.5, 21)
                line(3.5, 3.5, 20.5, 20.5)
                break

            case "theater":
                // 宽屏:一个横长矩形 + 左右两个反向箭头
                box(2, 6.5, 20, 11)
                line(10.5, 12, 6, 12)
                line(8.2, 9.8, 6, 12)
                line(6, 12, 8.2, 14.2)
                line(13.5, 12, 18, 12)
                line(15.8, 9.8, 18, 12)
                line(18, 12, 15.8, 14.2)
                break

            case "window-full":
                // 窗口全屏:像浏览器窗口,顶上有一条栏
                box(2, 4, 20, 16)
                line(2, 9, 22, 9)
                break

            case "screen-full":
                // 屏幕全屏:四个角括号
                poly([4, 9, 4, 4, 9, 4])
                poly([15, 4, 20, 4, 20, 9])
                poly([20, 15, 20, 20, 15, 20])
                poly([9, 20, 4, 20, 4, 15])
                break

            case "film":
                box(2, 4, 20, 16)
                line(7, 4, 7, 20)
                line(17, 4, 17, 20)
                line(2, 9, 7, 9)
                line(2, 15, 7, 15)
                line(17, 9, 22, 9)
                line(17, 15, 22, 15)
                break

            case "plus":
                line(12, 5, 12, 19)
                line(5, 12, 19, 12)
                break

            case "copy":
                box(9, 9, 11, 11)
                poly([5, 15, 5, 5, 15, 5])
                break

            case "image":
                box(2.5, 4.5, 19, 15)
                circle(8.5, 9.5, 1.6)
                poly([4, 17, 10, 11.5, 14.5, 16, 17, 13.5, 20, 17])
                break
            }

            ctx.restore()
        }
    }
}
