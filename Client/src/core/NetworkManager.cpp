//
// Created by donggu on 2026/9/7.
//

#include "NetworkManager.h"

#include "core/AppSettings.h"
#include "core/Log.h"

namespace {

// 单次连接的超时。WebSocket 没有内建超时，不自己掐的话要等 OS 级的 TCP
// 超时（几十秒）—— 那期间界面一直"连接中…",用户只能干等
constexpr int kConnectTimeoutMs = 8000;

// 客户端心跳。连接建立后每 10 秒 ping 一次；15 秒内没收到对应 pong 就
// 主动 abort。服务端（Beast）会自动回 pong，正常连接不会有额外负担。
// 没有心跳时，"网络静默黑洞"只能等 TCP 重传超时，可能要几分钟才会
// 触发 disconnected —— 那期间界面仍显示已连接，播放控制全部发不出去。
constexpr int kPingIntervalMs = 10 * 1000;
constexpr int kPongTimeoutMs = 15 * 1000;

// 整轮失败后的重试间隔：第一次 15 秒，之后每次翻倍，60 秒封顶
constexpr int kRetryBaseMs = 15 * 1000;
constexpr int kRetryMaxMs = 60 * 1000;

} // namespace

NetworkManager::NetworkManager(AppSettings *settings, QObject *parent)
    : QObject(parent),
      m_settings(settings),
      m_webSocket(std::make_unique<QWebSocket>()) {

    m_connectTimer.setSingleShot(true);
    m_retryTimer.setSingleShot(true);
    m_pongTimeout.setSingleShot(true);
    m_pingTimer.setInterval(kPingIntervalMs);
    m_pongTimeout.setInterval(kPongTimeoutMs);

    // 心跳发送：payload 用递增序号，pong 回来时据此判断是不是这一轮的。
    // 上一个 pong 还没回来时不重复发——m_pongTimeout 正在计时，超时就会断开。
    connect(&m_pingTimer, &QTimer::timeout, this, [this]() {
        if (!m_connected || m_pongTimeout.isActive())
            return;

        m_pingPayload = QByteArray::number(++m_pingCounter);
        LOG_TRACE("Network") << "发送心跳 ping" << m_pingPayload;
        m_webSocket->ping(m_pingPayload);
        m_pongTimeout.start();
    });

    connect(&m_pongTimeout, &QTimer::timeout, this, [this]() {
        if (!m_connected)
            return;

        LOG_WARN("Network") << "心跳超时，主动断开:" << m_currentUrl.toString();
        m_pingTimer.stop();
        abortSocket();
    });

    connect(&m_connectTimer, &QTimer::timeout, this, [this]() {
        if (m_connected)
            return;
        LOG_WARN("Network") << "连接超时:" << m_currentUrl.toString();
        handleAttemptFailed(QStringLiteral("连接超时"));
    });

    connect(&m_retryTimer, &QTimer::timeout, this, [this]() {
        if (m_userDisconnected)
            return;
        LOG_INFO("Network") << "重试连接(" << m_failedRounds + 1 << ")";
        startRound();
    });

    connect(
        m_webSocket.get(),
        &QWebSocket::connected,
        this,
        [this]() {
            stopTimers();
            m_connected = true;
            m_retrying = false;
            m_failedRounds = 0;
            // 记住这台服务器：之后掉线重连只回这里，不再尝试兜底地址
            m_hasPinnedUrl = true;
            m_pinnedUrl = m_currentUrl;
            LOG_INFO("Network") << "已连接:" << m_currentUrl.toString()
                                << (m_usingFallback ? "(内置地址)" : "(用户设置地址)");
            // 连上之后才开始心跳；连接尝试阶段不 ping。
            m_pingTimer.start();
            emit connected();
            emit isConnectedChanged();
            emit stateChanged();
        }
    );

    connect(m_webSocket.get(),
        &QWebSocket::disconnected,
        this,
        [this]() {
            // 连接**尝试**失败时 Qt 也可能发这个信号（各版本不一致）。
            // 只有"真的连上过再断"才算"掉线",否则由 handleAttemptFailed 处理
            if (!m_connected)
                return;

            m_connected = false;
            stopTimers();
            LOG_WARN("Network") << "与服务器的连接已断开:" << m_currentUrl.toString();
            emit disconnected();
            emit isConnectedChanged();
            emit stateChanged();

            if (!m_userDisconnected)
                scheduleRetry();
        }
    );

    connect(m_webSocket.get(),
        &QWebSocket::textMessageReceived,
        this,
        [this](const QString &message) {
            emit messageReceived(message);
        }
    );

    // 只认当前这一轮 ping 的 payload：旧 ping 的 pong 晚到不能抵消新的超时。
    connect(m_webSocket.get(), &QWebSocket::pong, this,
        [this](quint64 elapsedTime, const QByteArray &payload) {
            LOG_TRACE("Network") << "收到心跳 pong:" << payload
                                 << "往返" << elapsedTime << "ms";
            if (payload == m_pingPayload)
                m_pongTimeout.stop();
        }
    );

    connect(m_webSocket.get(),
        &QWebSocket::errorOccurred,
        this,
        [this](QAbstractSocket::SocketError error) {
            Q_UNUSED(error);

            // 自己 abort 触发的那一次不算失败（切换地址时必然发生）
            if (m_switching) {
                m_switching = false;
                return;
            }

            // 连上之后的收发错误不算"这次没连上";真掉线会走 disconnected
            if (m_connected)
                return;

            const QString reason = m_webSocket->errorString();
            LOG_WARN("Network") << "连接失败:" << m_currentUrl.toString() << reason;
            emit errorOccurred(reason);
            handleAttemptFailed(reason);
        }
    );
}

bool NetworkManager::isConnected() const {
    return m_connected && m_webSocket->state() == QAbstractSocket::ConnectedState;
}

bool NetworkManager::connecting() const {
    // 连接正在进行
    return m_connectTimer.isActive();
}

bool NetworkManager::retrying() const {
    return m_retrying;
}

QString NetworkManager::effectiveUrl() const {
    return m_currentUrl.toString();
}

bool NetworkManager::usingFallback() const {
    return m_usingFallback;
}

// ============================
// 连接
// ============================

void NetworkManager::connectToServer() {
    m_userDisconnected = false;
    m_failedRounds = 0;
    startRound();
}

void NetworkManager::retryNow() {
    m_userDisconnected = false;
    stopTimers();
    startRound();
}

void NetworkManager::disconnectFromServer() {
    m_userDisconnected = true;
    stopTimers();
    m_retrying = false;
    m_connected = false;
    m_hasPinnedUrl = false;
    abortSocket();
    emit isConnectedChanged();
    emit stateChanged();
}

void NetworkManager::clearServerPin() {
    m_hasPinnedUrl = false;
    m_pinnedUrl = QUrl();
}

void NetworkManager::startRound() {
    m_attempts.clear();

    // 已经成功连过一台服务器：掉线重连必须回到同一台。
    // 房间、clientId、resume token 都只属于那台服务器；切到兜底地址
    // 只会得到 bad_message / room_not_found。
    if (m_hasPinnedUrl && !m_pinnedUrl.isEmpty()) {
        LOG_INFO("Network") << "重连固定服务器:" << m_pinnedUrl.toString();
        m_attempts << m_pinnedUrl.toString();
        m_attemptIndex = 0;
        tryNextUrl();
        return;
    }

    const QString userUrl = m_settings != nullptr ? m_settings->serverUrl() : QString();
    const QString fallbackUrl = m_settings != nullptr ? m_settings->fallbackServerUrl()
                                                      : QString();

    // 用户地址优先；和兜底一样就不重复试一遍
    if (!userUrl.isEmpty() && userUrl != fallbackUrl)
        m_attempts << userUrl;
    if (!fallbackUrl.isEmpty())
        m_attempts << fallbackUrl;

    // 一个都没有（用户把地址清空、兜底又是空的）:别再空转
    if (m_attempts.isEmpty()) {
        LOG_ERROR("Network") << "没有可用的服务器地址";
        emit connectionFailed(QStringLiteral("没有可用的服务器地址"));
        return;
    }

    LOG_INFO("Network") << "开始连接,依次尝试:" << m_attempts.join(QStringLiteral(" → "));
    m_attemptIndex = 0;
    tryNextUrl();
}

void NetworkManager::tryNextUrl() {
    if (m_attemptIndex >= m_attempts.size()) {
        // 正常不会走到这儿（试完了由 handleAttemptFailed 安排重试）,
        // 留着只是防御 —— 别在这里再排一次重试，会和那边重复
        LOG_ERROR("Network") << "地址列表全部连接失败";
        return;
    }

    const QString next = m_attempts.at(m_attemptIndex++);
    const bool isFallback = m_settings != nullptr
                            && next == m_settings->fallbackServerUrl();

    if (isFallback && m_attemptIndex > 1)
        LOG_INFO("Network") << "用户设置地址连接失败，尝试内置地址:" << next;

    startAttempt(QUrl(next));
}

void NetworkManager::startAttempt(const QUrl &url) {
    if (!url.isValid() || (url.scheme() != QLatin1String("ws")
                           && url.scheme() != QLatin1String("wss"))) {
        LOG_ERROR("Network") << "地址不合法,跳过:" << url.toString();
        handleAttemptFailed(QStringLiteral("地址不合法"));
        return;
    }

    stopTimers();
    abortSocket();

    m_currentUrl = url;
    m_usingFallback = m_settings != nullptr
                      && url.toString() == m_settings->fallbackServerUrl();
    m_retrying = false;

    LOG_INFO("Network") << "连接" << url.toString();
    emit stateChanged();

    m_switching = false;
    m_webSocket->open(url);
    m_connectTimer.start(kConnectTimeoutMs);
}

void NetworkManager::handleAttemptFailed(const QString &reason) {
    stopTimers();
    abortSocket();

    if (m_attemptIndex < m_attempts.size()) {
        tryNextUrl();
        return;
    }

    // 整轮都失败了
    ++m_failedRounds;
    emit connectionFailed(reason);
    scheduleRetry();
}

void NetworkManager::scheduleRetry() {
    if (m_userDisconnected)
        return;

    m_retrying = true;
    emit stateChanged();

    // 15s → 30s → 60s 封顶
    int delay = kRetryBaseMs;
    for (int i = 1; i < m_failedRounds; ++i)
        delay = qMin(delay * 2, kRetryMaxMs);

    LOG_INFO("Network") << "将在" << delay / 1000 << "秒后重试";
    m_retryTimer.start(delay);
}

void NetworkManager::stopTimers() {
    m_connectTimer.stop();
    m_retryTimer.stop();
    m_pingTimer.stop();
    m_pongTimeout.stop();
}

void NetworkManager::abortSocket() {
    if (m_webSocket->state() == QAbstractSocket::UnconnectedState)
        return;

    // abort 之后 Qt 会补一个 errorOccurred(OperationCanceledError),
    // 那不是"连不上",置位让上面那个 lambda 把它吞掉
    m_switching = true;
    m_webSocket->abort();
}

bool NetworkManager::sendMessage(const QString &message) {
    if (!isConnected())
        return false;

    if (message.isEmpty())
        return false;

    m_webSocket->sendTextMessage(message);
    return true;
}
