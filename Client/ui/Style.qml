pragma Singleton

import QtQuick

// 全局设计令牌。颜色/圆角只在这里定义,组件和页面一律引用,
// 想调整整体风格(比如换主色)只改这一个文件。
//
// 2026-10-10 起整体换成 bilibili 蓝白:主色 #00AEEC,圆角压到 4px 以内。
QtObject {
    // ── 底色 ────────────────────────────────────────────
    readonly property color bg: "#F4F5F7"
    readonly property color card: "#FFFFFF"
    readonly property color border: "#E3E5E7"
    readonly property color fieldBg: "#F1F2F3"
    readonly property color fieldHover: "#E9EBEE"
    readonly property color hover: "#F7F8FA"

    // ── 文字 ────────────────────────────────────────────
    readonly property color textPrimary: "#18191C"
    readonly property color textSecondary: "#61666D"
    readonly property color textFaint: "#9499A0"
    readonly property color textOnAccent: "#FFFFFF"

    // ── 主色(蓝) ───────────────────────────────────────
    readonly property color accent: "#00AEEC"
    readonly property color accentHover: "#00A0DA"
    readonly property color accentPressed: "#0091C7"
    readonly property color accentSoft: "#E5F6FD"
    readonly property color accentLine: "#B3E5F7"

    // 品牌粉:只用于身份标记(房主徽标),不当主色用
    readonly property color brand: "#FB7299"
    readonly property color brandSoft: "#FFEDF3"

    readonly property color danger: "#FA5151"
    readonly property color success: "#21C25E"

    // ── 播放器(深色,自成一套,不跟页面主题走) ──────────
    // 控制条用纯色半透明,不用渐变
    readonly property color playerBg: "#000000"
    readonly property color playerBar: "#B8000000"
    readonly property color playerLine: "#1FFFFFFF"
    readonly property color playerIcon: "#FFFFFF"
    readonly property color playerIconDim: "#8AFFFFFF"
    readonly property color playerTrack: "#3DFFFFFF"
    readonly property color playerTrackBuf: "#5CFFFFFF"
    readonly property color playerField: "#24FFFFFF"

    // ── 圆角 / 尺寸 ─────────────────────────────────────
    readonly property int radius: 4
    readonly property int radiusSmall: 3
    readonly property int radiusTiny: 2
    readonly property int cardPadding: 24
    readonly property int pageWidth: 420
    readonly property int inputHeight: 44
    readonly property int topBarHeight: 52

    // 时间格式化。播放器的进度条和按钮行都要用,放这里避免两边各写一份。
    // 超过一小时会带小时位(电影动辄两三小时)。
    function formatTime(ms) {
        if (ms <= 0)
            return "00:00"
        const total = Math.floor(ms / 1000)
        const h = Math.floor(total / 3600)
        const m = Math.floor((total % 3600) / 60)
        const s = total % 60
        return (h > 0 ? h + ":" : "")
             + (h > 0 && m < 10 ? "0" : "") + m
             + ":" + (s < 10 ? "0" : "") + s
    }
}
