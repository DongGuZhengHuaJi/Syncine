#include "DouyuLiveDanmakuSession.h"

#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslError>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QWebSocket>
#include <QWebSocketProtocol>

#include "core/Log.h"

namespace {

constexpr int kHeartbeatMs = 15000;
constexpr int kAuthTimeoutMs = 8000;
constexpr int kReconnectBaseMs = 2000;
constexpr int kReconnectMaxMs = 15000;
constexpr int kMaxTextLength = 200;
constexpr int kMaxBatchItems = 50;
constexpr int kPortFirst = 8501;
constexpr int kPortCount = 6;
constexpr quint32 kPacketType = 689;
constexpr quint32 kServerPacketType = 690;

const char *kUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/154.0.0.0 Safari/537.36";

quint32 readLe32(const QByteArray &data, int offset)
{
    return (static_cast<quint32>(static_cast<uchar>(data.at(offset))))
           | (static_cast<quint32>(static_cast<uchar>(data.at(offset + 1))) << 8)
           | (static_cast<quint32>(static_cast<uchar>(data.at(offset + 2))) << 16)
           | (static_cast<quint32>(static_cast<uchar>(data.at(offset + 3))) << 24);
}

void appendLe32(QByteArray &data, quint32 value)
{
    data.append(static_cast<char>(value & 0xFF));
    data.append(static_cast<char>((value >> 8) & 0xFF));
    data.append(static_cast<char>((value >> 16) & 0xFF));
    data.append(static_cast<char>((value >> 24) & 0xFF));
}

// 斗鱼 STT 的转义：@A -> '@'，@S -> '/'
QString unescapeDouyu(QString value)
{
    value.replace(QStringLiteral("@A"), QStringLiteral("@"));
    value.replace(QStringLiteral("@S"), QStringLiteral("/"));
    return value;
}

// "type@=chatmsg/rid@=.../txt@=..." → { type: chatmsg, ... }
QHash<QString, QString> parseFields(const QString &body)
{
    QHash<QString, QString> fields;
    const QStringList parts = body.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const int separator = part.indexOf(QStringLiteral("@="));
        if (separator <= 0)
            continue;
        fields.insert(part.left(separator),
                      unescapeDouyu(part.mid(separator + 2)));
    }
    return fields;
}

QString colorFromField(const QString &value)
{
    bool ok = false;
    const uint color = value.toUInt(&ok);
    // 斗鱼的 col 有时是小的颜色序号（实测 2/6/7），不是 RGB；
    // 直接当 0x000002 会变成近黑色。小值按默认白色处理。
    if (!ok || color <= 0xFFFF)
        return QStringLiteral("#FFFFFF");
    return QStringLiteral("#%1").arg(color & 0xFFFFFFu, 6, 16, QLatin1Char('0'));
}

} // namespace

DouyuLiveDanmakuSession::DouyuLiveDanmakuSession(QObject *parent)
    : QObject(parent),
      m_network(new QNetworkAccessManager(this))
{
    m_heartbeatTimer.setInterval(kHeartbeatMs);
    connect(&m_heartbeatTimer, &QTimer::timeout,
            this, &DouyuLiveDanmakuSession::sendHeartbeat);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (!m_active)
            return;
        if (m_roomId.isEmpty())
            resolveRoomId();
        else
            openSocket();
    });
}

DouyuLiveDanmakuSession::~DouyuLiveDanmakuSession()
{
    // 析构时不再发 stopped/connectedChanged；只做静默清理。
    m_active = false;
    ++m_generation;
    ++m_connectionGeneration;
    m_reconnectTimer.stop();
    m_heartbeatTimer.stop();
    abortNetwork();
    if (m_socket != nullptr) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

// ============================
// 生命周期
// ============================

void DouyuLiveDanmakuSession::start(const QString &pageUrl,
                                     const QString &roomIdHint)
{
    stop();

    m_pageUrl = pageUrl.trimmed();
    if (m_pageUrl.isEmpty()) {
        fail(QStringLiteral("斗鱼弹幕缺少页面地址"));
        emit stopped();
        return;
    }

    static const QRegularExpression digits(QStringLiteral("^\\d+$"));
    const QString hint = roomIdHint.trimmed();
    m_roomId = digits.match(hint).hasMatch() ? hint : QString();

    m_active = true;
    ++m_generation;
    m_reconnectAttempt = 0;
    m_portIndex = 0;

    LOG_INFO("DouyuDanmaku") << "开始斗鱼直播弹幕:" << m_pageUrl
                             << "room_id:"
                             << (m_roomId.isEmpty() ? QStringLiteral("(页面解析)")
                                                    : m_roomId);

    if (m_roomId.isEmpty())
        resolveRoomId();
    else
        openSocket();
}

void DouyuLiveDanmakuSession::stop()
{
    const bool hadState = m_active || m_state != State::Idle
                          || m_socket != nullptr || !m_reply.isNull()
                          || m_heartbeatTimer.isActive()
                          || m_reconnectTimer.isActive();
    if (!hadState)
        return;

    m_active = false;
    ++m_generation;
    ++m_connectionGeneration;
    m_reconnectTimer.stop();
    m_heartbeatTimer.stop();
    abortNetwork();
    closeSocket();
    m_state = State::Idle;
    m_roomId.clear();
    m_pageUrl.clear();
    m_batch.clear();
    m_reconnectAttempt = 0;
    m_portIndex = 0;

    LOG_INFO("DouyuDanmaku") << "斗鱼直播弹幕会话已停止";
    emit stopped();
}

// ============================
// 页面 → 真实房间号
// ============================

void DouyuLiveDanmakuSession::resolveRoomId()
{
    if (!m_active)
        return;

    abortNetwork();
    m_reconnectTimer.stop();
    const quint64 generation = m_generation;
    m_state = State::ResolvingRoom;

    QNetworkRequest request{QUrl(m_pageUrl)};
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Referer", "https://www.douyu.com/");
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(10000);

    LOG_INFO("DouyuDanmaku") << "解析斗鱼页面房间号:" << m_pageUrl;
    QNetworkReply *reply = m_network->get(request);
    m_reply = reply;

    connect(reply, &QNetworkReply::finished, this, [this, reply, generation]() {
        const QByteArray data = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const QString errorText = reply->errorString();
        if (m_reply == reply)
            m_reply = nullptr;
        reply->deleteLater();

        if (generation != m_generation || !m_active)
            return;

        if (error != QNetworkReply::NoError) {
            fail(QStringLiteral("斗鱼页面请求失败: %1").arg(errorText));
            scheduleReconnect();
            return;
        }

        const QString html = QString::fromUtf8(data);
        static const QRegularExpression first(
            QStringLiteral(R"(window\.room_id\s*=\s*(\d+))"));
        static const QRegularExpression second(
            QStringLiteral(R"("room_id"\s*:\s*(\d+))"));
        QRegularExpressionMatch match = first.match(html);
        if (!match.hasMatch())
            match = second.match(html);
        if (!match.hasMatch()) {
            fail(QStringLiteral("斗鱼页面没有找到 room_id"));
            scheduleReconnect();
            return;
        }

        m_roomId = match.captured(1);
        m_portIndex = 0;
        LOG_INFO("DouyuDanmaku") << "真实房间号:" << m_roomId;
        openSocket();
    });
}

// ============================
// WebSocket
// ============================

void DouyuLiveDanmakuSession::openSocket()
{
    if (!m_active)
        return;
    if (m_roomId.isEmpty()) {
        resolveRoomId();
        return;
    }

    closeSocket();
    m_state = State::Connecting;
    const quint64 connectionGeneration = ++m_connectionGeneration;

    const int port = kPortFirst + (m_portIndex % kPortCount);
    ++m_portIndex;
    const QUrl url(QStringLiteral("wss://danmuproxy.douyu.com:%1/").arg(port));

    LOG_INFO("DouyuDanmaku") << "连接弹幕服务器:" << url.toString()
                             << "room_id:" << m_roomId;

    m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);

    connect(m_socket, &QWebSocket::connected, this,
            [this, connectionGeneration]() {
                if (connectionGeneration != m_connectionGeneration)
                    return;
                LOG_INFO("DouyuDanmaku") << "WebSocket 已连接，发送登录";
                sendLogin();
            });

    connect(m_socket, &QWebSocket::binaryMessageReceived, this,
            [this, connectionGeneration](const QByteArray &message) {
                if (connectionGeneration != m_connectionGeneration)
                    return;
                onBinaryMessage(message);
            });

    connect(m_socket, &QWebSocket::disconnected, this,
            [this, connectionGeneration]() {
                if (connectionGeneration != m_connectionGeneration)
                    return;
                LOG_INFO("DouyuDanmaku") << "弹幕连接断开";
                setConnected(false);
                m_heartbeatTimer.stop();
                if (m_active)
                    scheduleReconnect();
            });

    connect(m_socket, &QWebSocket::errorOccurred, this,
            [this, connectionGeneration](QAbstractSocket::SocketError) {
                if (connectionGeneration != m_connectionGeneration)
                    return;
                LOG_WARN("DouyuDanmaku") << "WebSocket 错误:"
                                         << m_socket->errorString();
            });

    connect(m_socket, &QWebSocket::sslErrors, this,
            [connectionGeneration, this](const QList<QSslError> &errors) {
                if (connectionGeneration != m_connectionGeneration)
                    return;
                for (const QSslError &error : errors)
                    LOG_WARN("DouyuDanmaku") << "TLS 错误:"
                                             << error.errorString();
            });

    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Origin", "https://www.douyu.com");
    request.setRawHeader("Referer", m_pageUrl.toUtf8());
    m_socket->open(request);

    QTimer::singleShot(kAuthTimeoutMs, this,
                       [this, connectionGeneration]() {
                           if (connectionGeneration != m_connectionGeneration)
                               return;
                           if (m_state != State::Connecting)
                               return;
                           LOG_WARN("DouyuDanmaku") << "弹幕认证超时";
                           closeSocket();
                           if (m_active)
                               scheduleReconnect();
                       });
}

void DouyuLiveDanmakuSession::sendLogin()
{
    if (m_socket == nullptr
        || m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    sendPacket(QStringLiteral("type@=loginreq/roomid@=%1/").arg(m_roomId));
}

void DouyuLiveDanmakuSession::sendJoin()
{
    sendPacket(QStringLiteral("type@=joingroup/rid@=%1/gid@=1/").arg(m_roomId));
}

void DouyuLiveDanmakuSession::sendHeartbeat()
{
    if (!m_connected)
        return;
    sendPacket(QStringLiteral("type@=mrkl/"));
}

void DouyuLiveDanmakuSession::sendPacket(const QString &body)
{
    if (m_socket == nullptr
        || m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    QByteArray payload = body.toUtf8();
    payload.append('\0');

    // 12 字节小端头：remaining(总长-4) 重复两次，然后 type=689
    const quint32 remaining = static_cast<quint32>(payload.size() + 8);
    QByteArray packet;
    packet.reserve(12 + payload.size());
    appendLe32(packet, remaining);
    appendLe32(packet, remaining);
    appendLe32(packet, kPacketType);
    packet.append(payload);

    m_socket->sendBinaryMessage(packet);
}

// ============================
// 收包
// ============================

void DouyuLiveDanmakuSession::onBinaryMessage(const QByteArray &message)
{
    m_batch.clear();

    int offset = 0;
    while (offset + 12 <= message.size()) {
        const quint32 remaining = readLe32(message, offset);
        const quint32 total = remaining + 4;
        if (total < 12
            || total > static_cast<quint32>(message.size() - offset)) {
            LOG_WARN("DouyuDanmaku") << "非法弹幕包: remaining=" << remaining
                                     << " available=" << (message.size() - offset);
            break;
        }

        const quint32 type = readLe32(message, offset + 8);
        QByteArray body = message.mid(offset + 12,
                                      static_cast<int>(total) - 12);
        while (body.endsWith('\0'))
            body.chop(1);

        if (type == kServerPacketType || type == kPacketType)
            handleMessage(QString::fromUtf8(body));
        else
            LOG_TRACE("DouyuDanmaku") << "未知包类型:" << type;

        offset += static_cast<int>(total);
    }

    if (!m_batch.isEmpty()) {
        LOG_TRACE("DouyuDanmaku") << "一批直播弹幕:" << m_batch.size();
        emit danmakuBatch(m_batch);
    }
}

void DouyuLiveDanmakuSession::handleMessage(const QString &message)
{
    if (message.isEmpty())
        return;

    const QHash<QString, QString> fields = parseFields(message);
    const QString type = fields.value(QStringLiteral("type"));

    if (type == QLatin1String("loginres")) {
        LOG_INFO("DouyuDanmaku") << "登录成功，加入弹幕组";
        sendJoin();
        sendHeartbeat();
        m_state = State::Connected;
        setConnected(true);
        return;
    }

    if (type == QLatin1String("pingreq")) {
        sendHeartbeat();
        return;
    }

    if (type != QLatin1String("chatmsg")) {
        LOG_TRACE("DouyuDanmaku") << "忽略消息类型:" << type;
        return;
    }

    QString text = fields.value(QStringLiteral("txt")).trimmed();
    if (text.isEmpty())
        return;
    if (text.size() > kMaxTextLength)
        text = text.left(kMaxTextLength);

    QVariantMap item;
    item.insert(QStringLiteral("time"), 0);
    item.insert(QStringLiteral("mode"), 1);
    item.insert(QStringLiteral("size"), 25);
    item.insert(QStringLiteral("color"),
                colorFromField(fields.value(QStringLiteral("col"))));
    item.insert(QStringLiteral("text"), text);

    if (m_batch.size() < kMaxBatchItems)
        m_batch.append(item);
}

// ============================
// 状态与重连
// ============================

void DouyuLiveDanmakuSession::setConnected(bool connected)
{
    if (m_connected == connected)
        return;

    m_connected = connected;
    if (connected) {
        m_reconnectAttempt = 0;
        m_heartbeatTimer.start();
    } else {
        m_heartbeatTimer.stop();
    }
    emit connectedChanged(connected);
}

void DouyuLiveDanmakuSession::scheduleReconnect()
{
    if (!m_active || m_reconnectTimer.isActive())
        return;

    const int exponent = qMin(m_reconnectAttempt, 3);
    const int delay = qMin(kReconnectMaxMs, kReconnectBaseMs * (1 << exponent));
    ++m_reconnectAttempt;
    LOG_INFO("DouyuDanmaku") << "将在" << delay << "ms 后重连";
    m_reconnectTimer.start(delay);
}

void DouyuLiveDanmakuSession::abortNetwork()
{
    if (m_reply.isNull())
        return;

    QNetworkReply *reply = m_reply.data();
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void DouyuLiveDanmakuSession::closeSocket()
{
    ++m_connectionGeneration;
    m_heartbeatTimer.stop();
    setConnected(false);

    if (m_socket == nullptr)
        return;

    QWebSocket *socket = m_socket;
    m_socket = nullptr;
    socket->disconnect(this);
    socket->close();
    socket->deleteLater();
}

void DouyuLiveDanmakuSession::fail(const QString &message)
{
    LOG_WARN("DouyuDanmaku") << message;
    emit errorOccurred(message);
}
