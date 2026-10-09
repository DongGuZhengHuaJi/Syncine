//
// Created by donggu on 2026/9/7.
//

#include "NetworkManager.h"

NetworkManager::NetworkManager(QObject *parent)
    : QObject(parent), m_webSocket(std::make_unique<QWebSocket>()) {

    connect(
        m_webSocket.get(),
        &QWebSocket::connected,
        this,
        [this]() {
            emit connected();
            emit isConnectedChanged();
        }
    );

    connect(m_webSocket.get(),
        &QWebSocket::disconnected,
        this,
        [this]() {
            emit disconnected();
            emit isConnectedChanged();
        }
    );

    connect(m_webSocket.get(),
        &QWebSocket::textMessageReceived,
        this,
        [this](const QString &message) {
            emit messageReceived(message);
        }
    );

    connect(m_webSocket.get(),
        &QWebSocket::errorOccurred,
        this,
        [this](QAbstractSocket::SocketError error) {
            Q_UNUSED(error);
            emit errorOccurred(m_webSocket->errorString());
        }
    );

    if (!isConnected()) {
        connectToServer();
    }

}

bool NetworkManager::isConnected() const {
    return m_webSocket->state() == QAbstractSocket::ConnectedState;
}

void NetworkManager::setServerUrl(const QString &serverUrl) {
    QUrl url(serverUrl);
    if (!url.isValid() || (url.scheme() != "ws" && url.scheme() != "wss")) {
        emit errorOccurred("Invalid WebSocket URL.");
        return;
    }

    if (m_serverUrl != url) {
        m_serverUrl = url;
        emit isConnectedChanged();
    }

    connectToServer();
}

void NetworkManager::connectToServer() {
    if (!m_serverUrl.isValid()) {
        emit errorOccurred("Invalid WebSocket URL.");
        return;
    }

    if (m_webSocket->state() != QAbstractSocket::UnconnectedState) {
        emit errorOccurred("WebSocket is already connecting or connected.");
        return;
    }

    m_webSocket->open(m_serverUrl);
}

void NetworkManager::disconnectFromServer() {
    m_webSocket->close();
}

bool NetworkManager::sendMessage(const QString &message) {
    if (m_webSocket->state() != QAbstractSocket::ConnectedState)
        return false;

    if (message.isEmpty())
        return false;

    m_webSocket->sendTextMessage(message);
    return true;
}
