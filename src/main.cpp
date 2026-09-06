#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include "PlaybackController.h"

int main(int argc, char *argv[]) {
    QGuiApplication a(argc, argv);

    QQmlApplicationEngine engine;

    PlaybackController playbackController;
    engine.rootContext()->setContextProperty("playbackController", & playbackController);

    engine.load(QUrl(QStringLiteral(
    "qrc:/qt/qml/SyncineApp/ui/Main.qml"
    )));


    return a.exec();
}
