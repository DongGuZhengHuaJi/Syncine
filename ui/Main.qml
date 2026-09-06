import QtQuick
import QtQuick.Controls
import SyncineApp

ApplicationWindow {
    visible: true

    width: 1200
    height: 800
    minimumWidth: 900
    minimumHeight: 640

    title: "Syncine"
    color: Style.bg

    StackView {
        id: stackView
        anchors.fill: parent

        initialItem: HomePage {}
    }
}
