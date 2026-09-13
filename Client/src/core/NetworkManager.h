//
// Created by donggu on 2026/9/7.
//

#ifndef SYNCINE_NETWORKMANAGER_H
#define SYNCINE_NETWORKMANAGER_H

#include <QObject>
#include <QWebSocket>
#include <memory>

class NetworkManager: public QObject{
    Q_OBJECT

    Q_PROPERTY(bool isConnected
               READ isConnected
               NOTIFY isConnectedChanged)

public:
    explicit NetworkManager(QObject *parent = nullptr);

    bool isConnected() const;

    Q_INVOKABLE void setServerUrl(const QString &serverUrl);
    Q_INVOKABLE void connectToServer();
    Q_INVOKABLE void disconnectFromServer();

    // 发送失败(未连接 / 空消息)返回 false 且不发信号,
    // 由调用方决定怎么向上报告 —— 传输层不该猜业务的错误文案
    bool sendMessage(const QString &message);

signals:
    void connected();
    void disconnected();
    void isConnectedChanged();
    void messageReceived(const QString &message);
    void errorOccurred(const QString &errorString);

private:
    std::unique_ptr<QWebSocket> m_webSocket;
    QUrl m_serverUrl{QStringLiteral("ws://localhost:8765")};
};


#endif //SYNCINE_NETWORKMANAGER_H
