import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia

RowLayout {
    id: root

    required property MediaPlayer mediaPlayer

    signal interacted()

    spacing: 10

    Label {
        text: formatTime(root.mediaPlayer.position)
        Layout.preferredWidth: 42
        horizontalAlignment: Text.AlignRight
        color: "#FFFFFF"
        font.pixelSize: 12
    }

    Slider {
        id: mediaSlider

        Layout.fillWidth: true

        enabled: root.mediaPlayer.seekable

        from: 0
        to: 1
        value: 0

        Binding {
            target: mediaSlider
            property: "value"
            value: root.mediaPlayer.duration > 0
                ? root.mediaPlayer.position / root.mediaPlayer.duration
                : 0
            when: !mediaSlider.pressed
        }

        onPressedChanged: {
            if (mediaSlider.pressed)
                root.interacted()
        }
        onMoved: {
            root.mediaPlayer.setPosition(
                value * root.mediaPlayer.duration
            )
            root.interacted()
        }

        background: Rectangle {
            x: mediaSlider.leftPadding
            y: mediaSlider.topPadding + mediaSlider.availableHeight / 2 - height / 2
            implicitWidth: 120
            implicitHeight: 4
            width: mediaSlider.availableWidth
            height: implicitHeight
            radius: 2
            color: "#4DFFFFFF"

            Rectangle {
                width: mediaSlider.visualPosition * parent.width
                height: parent.height
                radius: 2
                color: "#FFFFFF"
            }
        }

        handle: Rectangle {
            x: mediaSlider.leftPadding + mediaSlider.visualPosition * (mediaSlider.availableWidth - width)
            y: mediaSlider.topPadding + mediaSlider.availableHeight / 2 - height / 2
            implicitWidth: 14
            implicitHeight: 14
            radius: 7
            color: "#FFFFFF"
        }
    }

    Label {
        text: formatTime(root.mediaPlayer.duration)
        Layout.preferredWidth: 42
        color: "#FFFFFF"
        font.pixelSize: 12
    }

    function formatTime(ms) {
        if (ms <= 0)
            return "00:00"

        const totalSeconds = Math.floor(ms / 1000)
        const minutes = Math.floor(totalSeconds / 60)
        const seconds = totalSeconds % 60

        return (minutes < 10 ? "0" : "") + minutes
            + ":"
            + (seconds < 10 ? "0" : "") + seconds
    }
}
