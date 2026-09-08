import QtQuick
import SyncineApp

Item {
    id: root

    default property alias content: bodyContent.data
    property int padding: Style.cardPadding
    property color backgroundColor: Style.card
    property bool shadowed: true

    implicitWidth: bodyContent.childrenRect.width + padding * 2
    implicitHeight: bodyContent.childrenRect.height + padding * 2

    Rectangle {
        visible: root.shadowed
        anchors.fill: body
        anchors.topMargin: 8
        radius: Style.radius
        color: "#1B2434"
        opacity: 0.05
    }

    Rectangle {
        id: body
        anchors.fill: parent
        radius: Style.radius
        color: root.backgroundColor
        border.width: 1
        border.color: Style.border
    }

    Item {
        id: bodyContent
        anchors.fill: body
        anchors.margins: root.padding
    }
}
