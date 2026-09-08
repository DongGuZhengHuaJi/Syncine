import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia

Rectangle {
    id: root

    signal interacted()
    signal seeked(real position)

    implicitHeight: 64
    color: "#E6161922"

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onPositionChanged: root.interacted()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        spacing: 8

        PlaybackSeekControl {
            Layout.fillWidth: true
            onInteracted: root.interacted()
            onSeekFinished: root.seeked(position)
        }

        Item { Layout.preferredWidth: 8 }

        Button {
            id: backBtn
            width: 40
            height: 40
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40

            contentItem: Label {
                text: "-10"
                color: "#FFFFFF"
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                radius: 20
                color: backBtn.hovered ? "#2EFFFFFF" : "transparent"
            }

            onClicked: {
                playbackController.seekRelative(-10000)
                root.seeked(playbackController.position)
                root.interacted()
            }
        }

        Button {
            id: playPauseBtn
            width: 44
            height: 44
            Layout.preferredWidth: 44
            Layout.preferredHeight: 44

            contentItem: Canvas {
                id: playPauseBtnCanvas
                anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.fillStyle = "#FFFFFF"
                    ctx.clearRect(0, 0, width, height)
                    if (playbackController.playing) {
                        ctx.fillRect(15, 12, 6, 20)
                        ctx.fillRect(23, 12, 6, 20)
                    } else {
                        ctx.beginPath()
                        ctx.moveTo(17, 11)
                        ctx.lineTo(32, 22)
                        ctx.lineTo(17, 33)
                        ctx.closePath()
                        ctx.fill()
                    }
                }

                Connections {
                    target: playbackController
                    function onPlayingChanged() {
                        playPauseBtnCanvas.requestPaint()
                    }
                }
            }

            background: Rectangle {
                radius: 22
                color: playPauseBtn.hovered ? "#2EFFFFFF" : "#1FFFFFFF"
            }

            onClicked: {
                playbackController.togglePlayPause()
                root.interacted()
            }
        }

        Button {
            id: forwardBtn
            width: 40
            height: 40
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40

            contentItem: Label {
                text: "+10"
                color: "#FFFFFF"
                font.pixelSize: 13
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                radius: 20
                color: forwardBtn.hovered ? "#2EFFFFFF" : "transparent"
            }

            onClicked: {
                playbackController.seekRelative(10000)
                root.seeked(playbackController.position)
                root.interacted()
            }
        }

        Button {
            id: muteBtn
            width: 40
            height: 40
            Layout.preferredWidth: 40
            Layout.preferredHeight: 40

            contentItem: Canvas {
                id: muteBtnCanvas
                anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.fillStyle = "#FFFFFF"
                    ctx.strokeStyle = "#FFFFFF"
                    ctx.lineWidth = 2
                    ctx.clearRect(0, 0, width, height)

                    ctx.beginPath()
                    ctx.moveTo(12, 16)
                    ctx.lineTo(17, 16)
                    ctx.lineTo(23, 11)
                    ctx.lineTo(23, 29)
                    ctx.lineTo(17, 24)
                    ctx.lineTo(12, 24)
                    ctx.closePath()
                    ctx.fill()

                    if (playbackController.muted) {
                        ctx.beginPath()
                        ctx.moveTo(26, 15)
                        ctx.lineTo(34, 25)
                        ctx.moveTo(34, 15)
                        ctx.lineTo(26, 25)
                        ctx.stroke()
                    } else {
                        ctx.beginPath()
                        ctx.arc(29, 20, 4, -1.0, 1.0)
                        ctx.stroke()
                    }
                }

                Connections {
                    target: playbackController
                    function onMutedChanged() {
                        muteBtnCanvas.requestPaint()
                    }
                }
            }

            background: Rectangle {
                radius: 20
                color: muteBtn.hovered ? "#2EFFFFFF" : "transparent"
            }

            onClicked: {
                playbackController.toggleMute()
                root.interacted()
            }
        }

        Slider {
            id: volumeSlider
            Layout.preferredWidth: 90
            Layout.preferredHeight: 20

            from: 0
            to: 1
            value: playbackController.volume

            onPressedChanged: {
                if (volumeSlider.pressed)
                    root.interacted()
            }
            onMoved: {
                playbackController.volume = value
                root.interacted()
            }

            background: Rectangle {
                x: volumeSlider.leftPadding
                y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                implicitWidth: 90
                implicitHeight: 4
                width: volumeSlider.availableWidth
                height: implicitHeight
                radius: 2
                color: "#4DFFFFFF"

                Rectangle {
                    width: volumeSlider.visualPosition * parent.width
                    height: parent.height
                    radius: 2
                    color: "#FFFFFF"
                }
            }

            handle: Rectangle {
                x: volumeSlider.leftPadding + volumeSlider.visualPosition * (volumeSlider.availableWidth - width)
                y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                implicitWidth: 14
                implicitHeight: 14
                radius: 7
                color: "#FFFFFF"
            }
        }
    }
}
