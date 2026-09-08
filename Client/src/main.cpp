#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "PlaybackController.h"
#include "NetworkManager.h"
#include "RoomManager.h"

int main(int argc, char *argv[]) {
    QGuiApplication a(argc, argv);

    QQmlApplicationEngine engine;

    PlaybackController playbackController;
    NetworkManager networkManager;
    RoomManager roomManager;

    // 让 RoomManager 通过 NetworkManager 收发协议消息
    roomManager.setNetworkManager(&networkManager);

    engine.rootContext()->setContextProperty("playbackController", & playbackController);
    engine.rootContext()->setContextProperty("networkManager", & networkManager);
    engine.rootContext()->setContextProperty("roomManager", & roomManager);

    engine.load(QUrl(QStringLiteral(
    "qrc:/qt/qml/SyncineApp/ui/Main.qml"
    )));


    return a.exec();
}
