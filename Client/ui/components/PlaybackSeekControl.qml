import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

RowLayout {
    id: root

    signal interacted()
    signal seekFinished(real position)

    spacing: 10

    Label {
        text: formatTime(playbackController.position)
        Layout.preferredWidth: 42
        horizontalAlignment: Text.AlignRight
        color: "#FFFFFF"
        font.pixelSize: 12
    }

    Slider {
        id: mediaSlider

        Layout.fillWidth: true

        enabled: playbackController.seekable

        from: 0
        to: 1
        value: 0

        Binding {
            target: mediaSlider
            property: "value"
            value: playbackController.duration > 0
                ? playbackController.position / playbackController.duration
                : 0
            when: !mediaSlider.pressed
        }

        onPressedChanged: {
            if (!mediaSlider.pressed) {
                const position = mediaSlider.value * playbackController.duration
                playbackController.seek(position)
                root.seekFinished(position)
            }
            root.interacted()
        }
        onMoved: {
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
        text: formatTime(playbackController.duration)
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
