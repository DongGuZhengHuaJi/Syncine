#include <QGuiApplication>
#include <QQmlApplicationEngine>

int main(int argc, char *argv[]) {
    QGuiApplication a(argc, argv);

    QQmlApplicationEngine engine;
    engine.load(QUrl(QStringLiteral(
    "qrc:/qt/qml/SyncineApp/ui/Main.qml"
    )));

    return a.exec();
}
