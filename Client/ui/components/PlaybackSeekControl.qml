import QtQuick
import QtQuick.Controls
import SyncineApp

// 播放进度条:独占控制区的一整行、和播放器同宽。
//
// 时间显示不在这里 —— 在下面的按钮行里(Style.formatTime)。
// 这样进度条能拿到整行宽度,拖动精度也更高。
Slider {
    id: root

    signal interacted()

    height: 18
    enabled: playbackSync.seekable

    from: 0
    to: 1
    value: 0

    // 播放位置一直在变,但用户按着滑条的时候不能被它拽回去 ——
    // 这就是 when: !pressed 的作用。
    Binding {
        target: root
        property: "value"
        value: playbackSync.duration > 0
               ? playbackSync.position / playbackSync.duration
               : 0
        when: !root.pressed
    }

    onPressedChanged: {
        // 松手才真正跳转,拖动过程中不反复 seek
        if (!root.pressed)
            playbackSync.seek(root.value * playbackSync.duration)
        root.interacted()
    }
    onMoved: root.interacted()

    background: Rectangle {
        x: root.leftPadding
        y: root.topPadding + root.availableHeight / 2 - height / 2
        implicitWidth: 120
        implicitHeight: 3
        width: root.availableWidth
        height: implicitHeight
        color: Style.playerTrack

        Rectangle {
            width: root.visualPosition * parent.width
            height: parent.height
            color: Style.accent
        }
    }

    handle: Rectangle {
        x: root.leftPadding + root.visualPosition * (root.availableWidth - width)
        y: root.topPadding + root.availableHeight / 2 - height / 2
        implicitWidth: 11
        implicitHeight: 11
        radius: 5.5
        color: "#FFFFFF"
        opacity: root.enabled ? 1 : 0.4
    }
}
