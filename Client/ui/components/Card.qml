import QtQuick
import SyncineApp

// 白卡片:一整块白底 + 1px 边框,没有阴影。
//
// 2026-10-10 去掉了原来那层偏移 8px 的投影 —— 浅色扁平风里
// 投影是"AI 味"的主要来源,边框已经足够把卡片和页面底分开。
Item {
    id: root

    default property alias content: bodyContent.data
    property int padding: Style.cardPadding
    property color backgroundColor: Style.card

    implicitWidth: bodyContent.childrenRect.width + padding * 2
    implicitHeight: bodyContent.childrenRect.height + padding * 2

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
