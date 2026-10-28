#include "LiveDanmakuSession.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslError>
#include <QUrl>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWebSocketProtocol>

#include <zlib.h>

#include "core/Log.h"

namespace {

constexpr int kMaxPacketBytes = 16 * 1024 * 1024;
constexpr int kMaxTextLength = 500;
constexpr int kHeartbeatMs = 30000;
constexpr int kAuthTimeoutMs = 10000;
constexpr int kReconnectBaseMs = 3000;
constexpr int kReconnectMaxMs = 30000;

quint16 readBe16(const QByteArray &data, int offset)
{
    return static_cast<quint16>((static_cast<quint8>(data.at(offset)) << 8)
                                | static_cast<quint8>(data.at(offset + 1)));
}

quint32 readBe32(const QByteArray &data, int offset)
{
    return (static_cast<quint32>(static_cast<quint8>(data.at(offset))) << 24)
           | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 1))) << 16)
           | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 2))) << 8)
           | static_cast<quint32>(static_cast<quint8>(data.at(offset + 3)));
}

QByteArray makePacket(quint32 operation, const QByteArray &body,
                      quint16 version, quint32 sequence)
{
    const quint32 total = static_cast<quint32>(16 + body.size());
    QByteArray packet;
    packet.resize(static_cast<int>(total));

    auto put16 = [&packet](int offset, quint16 value) {
        packet[offset] = static_cast<char>((value >> 8) & 0xFF);
        packet[offset + 1] = static_cast<char>(value & 0xFF);
    };
    auto put32 = [&packet](int offset, quint32 value) {
        packet[offset] = static_cast<char>((value >> 24) & 0xFF);
        packet[offset + 1] = static_cast<char>((value >> 16) & 0xFF);
        packet[offset + 2] = static_cast<char>((value >> 8) & 0xFF);
        packet[offset + 3] = static_cast<char>(value & 0xFF);
    };

    put32(0, total);
    put16(4, 16);
    put16(6, version);
    put32(8, operation);
    put32(12, sequence);
    if (!body.isEmpty())
        packet.replace(16, body.size(), body);
    return packet;
}

QString jsonId(const QJsonValue &value)
{
    if (value.isString())
        return value.toString();
    if (value.isDouble())
        return QString::number(static_cast<qint64>(value.toDouble()));
    return {};
}

QString shortRoomIdFromUrl(const QString &url)
{
    static const QRegularExpression patterns[] = {
        QRegularExpression(QStringLiteral(R"(live\.bilibili\.com/(?:blanc/)?(\d+))")),
        QRegularExpression(QStringLiteral(R"([?&]room_id=(\d+))")),
    };
    for (const QRegularExpression &expression : patterns) {
        const QRegularExpressionMatch match = expression.match(url);
        if (match.hasMatch())
            return match.captured(1);
    }
    return {};
}

QByteArray inflateZlib(const QByteArray &data, bool *ok)
{
    *ok = false;
    if (data.isEmpty())
        return {};

    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    stream.avail_in = static_cast<uInt>(data.size());
    if (inflateInit(&stream) != Z_OK)
        return {};

    QByteArray output;
    char buffer[64 * 1024];
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef *>(buffer);
        stream.avail_out = sizeof(buffer);
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            inflateEnd(&stream);
            return {};
        }
        output.append(buffer, static_cast<int>(sizeof(buffer) - stream.avail_out));
        if (output.size() > kMaxPacketBytes) {
            inflateEnd(&stream);
            return {};
        }
    }
    inflateEnd(&stream);

    if (result != Z_STREAM_END)
        return {};
    *ok = true;
    return output;
}

QNetworkRequest makeApiRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent",
                         "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                         "(KHTML, like Gecko) Chrome/130.0 Safari/537.36");
    request.setRawHeader("Referer", "https://live.bilibili.com/");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(10000);
    return request;
}

} // namespace

LiveDanmakuSession::LiveDanmakuSession(const QString &cookieFilePath,
                                       QObject *parent)
    : QObject(parent),
      m_network(new QNetworkAccessManager(this)),
      m_authNetwork(new QNetworkAccessManager(this)),
      m_cookiePath(cookieFilePath)
{
    m_heartbeatTimer.setInterval(kHeartbeatMs);
    connect(&m_heartbeatTimer, &QTimer::timeout,
            this, &LiveDanmakuSession::sendHeartbeat);

    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_active)
            resolveRoomId();
    });
}

LiveDanmakuSession::~LiveDanmakuSession()
{
    // 控制器析构时不应再收到 stopped/connectedChanged；这里只做静默清理。
    m_active = false;
    ++m_generation;
    ++m_connectionGeneration;
    m_reconnectTimer.stop();
    m_heartbeatTimer.stop();
    abortApi();
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

void LiveDanmakuSession::start(const QString &pageUrl)
{
    stop();

    m_pageUrl = pageUrl;
    m_shortRoomId = shortRoomIdFromUrl(pageUrl);
    if (m_shortRoomId.isEmpty()) {
        fail(QStringLiteral("无法从链接解析直播间号: %1").arg(pageUrl));
        emit stopped();
        return;
    }

    m_active = true;
    ++m_generation;
    m_reconnectAttempt = 0;
    m_cookieCount = loadCookies();
    LOG_INFO("LiveDanmaku") << "开始直播间弹幕:" << m_shortRoomId
                            << " cookie:" << m_cookieCount << "条"
                            << " 登录态:" << (m_uid > 0 ? "已登录" : "匿名");
    resolveRoomId();
}

void LiveDanmakuSession::stop()
{
    const bool hadState = m_active || m_state != State::Idle
                          || m_socket != nullptr || !m_reply.isNull()
                          || m_heartbeatTimer.isActive()
                          || m_reconnectTimer.isActive();
    if (!hadState)
        return;

    m_active = false;
    ++m_generation;
    m_reconnectTimer.stop();
    abortApi();
    closeSocket();
    m_heartbeatTimer.stop();
    m_state = State::Idle;
    m_shortRoomId.clear();
    m_roomId.clear();
    m_token.clear();
    m_hosts.clear();
    m_hostIndex = 0;
    m_reconnectAttempt = 0;
    m_batch.clear();

    LOG_INFO("LiveDanmaku") << "直播弹幕会话已停止";
    emit stopped();
}

// ============================
// API：短号 → 真实房间号 → token/主机
// ============================

void LiveDanmakuSession::resolveRoomId()
{
    if (!m_active)
        return;

    abortApi();
    m_reconnectTimer.stop();
    const quint64 generation = m_generation;
    m_state = State::ResolvingRoom;

    QUrl url(QStringLiteral("https://api.live.bilibili.com/room/v1/Room/room_init"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), m_shortRoomId);
    url.setQuery(query);

    LOG_INFO("LiveDanmaku") << "解析直播间:" << m_shortRoomId;
    QNetworkReply *reply = apiNetwork()->get(makeApiRequest(url));
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
            fail(QStringLiteral("room_init 请求失败: %1").arg(errorText));
            scheduleReconnect();
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(data).object();
        if (root.value(QStringLiteral("code")).toInt(-1) != 0) {
            fail(QStringLiteral("room_init 返回 code=%1")
                     .arg(root.value(QStringLiteral("code")).toInt()));
            scheduleReconnect();
            return;
        }

        const QJsonObject info = root.value(QStringLiteral("data")).toObject();
        m_roomId = jsonId(info.value(QStringLiteral("room_id")));
        if (m_roomId.isEmpty()) {
            fail(QStringLiteral("room_init 没有返回 room_id"));
            scheduleReconnect();
            return;
        }

        if (info.value(QStringLiteral("live_status")).toInt(0) != 1) {
            LOG_INFO("LiveDanmaku") << "直播间当前未开播:" << m_roomId;
            stop();
            return;
        }

        fetchConf();
    });
}

void LiveDanmakuSession::fetchConf()
{
    if (!m_active)
        return;

    abortApi();
    const quint64 generation = m_generation;
    m_state = State::FetchingConf;

    QUrl url(QStringLiteral("https://api.live.bilibili.com/room/v1/Danmu/getConf"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("room_id"), m_roomId);
    query.addQueryItem(QStringLiteral("platform"), QStringLiteral("pc"));
    query.addQueryItem(QStringLiteral("player"), QStringLiteral("web"));
    url.setQuery(query);

    LOG_INFO("LiveDanmaku") << "获取弹幕服务器, room_id=" << m_roomId;
    QNetworkReply *reply = apiNetwork()->get(makeApiRequest(url));
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
            fail(QStringLiteral("getConf 请求失败: %1").arg(errorText));
            scheduleReconnect();
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(data).object();
        if (root.value(QStringLiteral("code")).toInt(-1) != 0) {
            fail(QStringLiteral("getConf 返回 code=%1")
                     .arg(root.value(QStringLiteral("code")).toInt()));
            scheduleReconnect();
            return;
        }

        const QJsonObject info = root.value(QStringLiteral("data")).toObject();
        m_token = info.value(QStringLiteral("token")).toString();
        m_hosts.clear();
        const QJsonArray hosts = info.value(QStringLiteral("host_server_list")).toArray();
        for (const QJsonValue &value : hosts) {
            const QJsonObject host = value.toObject();
            const QString name = host.value(QStringLiteral("host")).toString();
            const int port = host.value(QStringLiteral("wss_port")).toInt(0);
            if (!name.isEmpty() && port > 0) {
                m_hosts.append(QStringLiteral("wss://%1:%2/sub").arg(name).arg(port));
            }
        }
        if (m_hosts.isEmpty()) {
            m_hosts.append(
                QStringLiteral("wss://broadcastlv.chat.bilibili.com:443/sub"));
        }
        m_hostIndex = 0;

        openSocket();
    });
}

int LiveDanmakuSession::loadCookies()
{
    // 每次 start 都先清掉上一场的登录态，避免 cookie 文件读取失败时
    // 留下旧的 uid 与匿名 token 混用
    m_uid = 0;

    if (m_cookiePath.isEmpty() || m_authNetwork == nullptr)
        return 0;

    // 每次 start 重建 cookie jar，避免上一场直播的旧 cookie 残留
    auto *jar = new QNetworkCookieJar(m_authNetwork);
    m_authNetwork->setCookieJar(jar);
    m_cookieJar = jar;

    QFile file(m_cookiePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return 0;

    int loaded = 0;
    const QStringList lines =
        QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;

        // Netscape 格式的 HttpOnly cookie 以 "#HttpOnly_" 开头，仍是有效行
        bool httpOnly = false;
        if (line.startsWith(QLatin1String("#HttpOnly_"))) {
            httpOnly = true;
            line = line.mid(10);
        } else if (line.startsWith(QLatin1Char('#'))) {
            continue;
        }

        const QStringList fields = line.split(QLatin1Char('\t'));
        if (fields.size() < 7)
            continue;
        const QString domain = fields.at(0);
        if (!domain.contains(QLatin1String("bilibili.com"),
                             Qt::CaseInsensitive)) {
            continue;
        }

        const QString path = fields.at(2).isEmpty() ? QStringLiteral("/")
                                                    : fields.at(2);
        const QString name = fields.at(5);
        const QString value = fields.at(6);
        if (name.isEmpty())
            continue;

        // uid 必须与 token 来自同一个 Cookie 账号，否则 WebSocket 会被服务器断开
        if (name == QLatin1String("DedeUserID")) {
            bool idOk = false;
            const qint64 id = value.toLongLong(&idOk);
            if (idOk)
                m_uid = id;
        }

        QNetworkCookie cookie(name.toUtf8(), value.toUtf8());
        cookie.setDomain(domain);
        cookie.setPath(path);
        cookie.setSecure(fields.at(3).compare(QLatin1String("TRUE"),
                                              Qt::CaseInsensitive) == 0);
        cookie.setHttpOnly(httpOnly);

        bool ok = false;
        const qint64 expires = fields.at(4).toLongLong(&ok);
        if (ok && expires > 0)
            cookie.setExpirationDate(QDateTime::fromSecsSinceEpoch(expires));

        // setCookiesFromUrl 会做 domain 校验；用 cookie 自己的域名构造 URL
        QString host = domain;
        if (host.startsWith(QLatin1Char('.')))
            host = host.mid(1);
        QUrl url;
        url.setScheme(QStringLiteral("https"));
        url.setHost(host);
        url.setPath(path);

        if (m_cookieJar->setCookiesFromUrl({cookie}, url))
            ++loaded;
    }

    LOG_DEBUG("LiveDanmaku") << "从 cookie 文件载入 bilibili cookie:"
                             << loaded << "条, uid"
                             << (m_uid > 0 ? "已找到" : "未找到");
    return loaded;
}

QNetworkAccessManager *LiveDanmakuSession::apiNetwork() const
{
    // 有登录 uid 时用带 cookie jar 的 QNAM，让 getConf 返回的 token 与账号绑定；
    // 没有登录态时退回匿名网络。
    return (m_uid > 0 && m_authNetwork != nullptr) ? m_authNetwork : m_network;
}

// WebSocket 握手用的 Cookie 头（QWebSocket 不会使用 QNAM 的 cookie jar）
QByteArray LiveDanmakuSession::cookieFileHeader() const
{
    if (m_cookiePath.isEmpty())
        return {};

    QFile file(m_cookiePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QStringList pairs;
    const QStringList lines =
        QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1String("#HttpOnly_")))
            line = line.mid(10);
        else if (line.startsWith(QLatin1Char('#')))
            continue;

        const QStringList fields = line.split(QLatin1Char('\t'));
        if (fields.size() < 7)
            continue;
        if (!fields.at(0).contains(QLatin1String("bilibili.com"),
                                   Qt::CaseInsensitive))
            continue;
        pairs.append(fields.at(5) + QLatin1Char('=') + fields.at(6));
    }

    return pairs.join(QStringLiteral("; ")).toUtf8();
}

// ============================
// WebSocket
// ============================

void LiveDanmakuSession::openSocket()
{
    if (!m_active)
        return;

    closeSocket();
    m_state = State::Connecting;
    // closeSocket 已经递增过一次；这里再取一个新值，旧 socket 的排队回调一律作废
    const quint64 connectionGeneration = ++m_connectionGeneration;

    const QString url = m_hosts.at(m_hostIndex % m_hosts.size());
    ++m_hostIndex;

    LOG_INFO("LiveDanmaku") << "连接弹幕服务器:" << url;

    m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);

    connect(m_socket, &QWebSocket::connected, this,
            [this, connectionGeneration]() {
                if (connectionGeneration != m_connectionGeneration)
                    return;
                sendAuth();
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
                LOG_INFO("LiveDanmaku") << "弹幕连接断开";
                setConnected(false);
                m_heartbeatTimer.stop();
                if (m_active)
                    scheduleReconnect();
            });

    connect(m_socket, &QWebSocket::errorOccurred, this,
            [this, connectionGeneration](QAbstractSocket::SocketError) {
                if (connectionGeneration != m_connectionGeneration)
                    return;
                LOG_WARN("LiveDanmaku") << "WebSocket 错误:" << m_socket->errorString();
            });

    connect(m_socket, &QWebSocket::sslErrors, this,
            [connectionGeneration, this](const QList<QSslError> &errors) {
                if (connectionGeneration != m_connectionGeneration)
                    return;
                for (const QSslError &error : errors)
                    LOG_WARN("LiveDanmaku") << "TLS 错误:" << error.errorString();
            });

    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("User-Agent",
                         "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                         "(KHTML, like Gecko) Chrome/130.0 Safari/537.36");
    request.setRawHeader("Origin", "https://live.bilibili.com");
    request.setRawHeader("Referer", "https://live.bilibili.com/");
    // 实测：纯匿名 WebSocket 会被明显限流；带上账号 Cookie 后同一房间
    // 20 秒内的弹幕量从 1 条提升到 12～16 条。
    const QByteArray cookies = cookieFileHeader();
    if (!cookies.isEmpty())
        request.setRawHeader("Cookie", cookies);
    m_socket->open(request);

    // 认证超时：连上了但一直没收到 op=8
    QTimer::singleShot(kAuthTimeoutMs, this, [this, connectionGeneration]() {
        if (connectionGeneration != m_connectionGeneration)
            return;
        if (m_state != State::Connecting)
            return;
        LOG_WARN("LiveDanmaku") << "弹幕认证超时";
        closeSocket();
        if (m_active)
            scheduleReconnect();
    });
}

void LiveDanmakuSession::sendAuth()
{
    if (m_socket == nullptr || m_socket->state() != QAbstractSocket::ConnectedState)
        return;

    QJsonObject auth;
    auth.insert(QStringLiteral("uid"), m_uid);
    auth.insert(QStringLiteral("roomid"), m_roomId.toLongLong());
    auth.insert(QStringLiteral("protover"), 2);
    auth.insert(QStringLiteral("platform"), QStringLiteral("web"));
    auth.insert(QStringLiteral("type"), 2);
    auth.insert(QStringLiteral("key"), m_token);

    sendPacket(7, QJsonDocument(auth).toJson(QJsonDocument::Compact), 1);
    LOG_INFO("LiveDanmaku") << "认证已发送, room_id=" << m_roomId;
}

void LiveDanmakuSession::sendHeartbeat()
{
    if (!m_connected)
        return;
    sendPacket(2, QByteArrayLiteral("[object Object]"), 1);
}

void LiveDanmakuSession::sendPacket(quint32 operation, const QByteArray &body,
                                    quint16 version)
{
    if (m_socket == nullptr || m_socket->state() != QAbstractSocket::ConnectedState)
        return;
    m_socket->sendBinaryMessage(makePacket(operation, body, version, ++m_sequence));
}

// ============================
// 收包与解析
// ============================

void LiveDanmakuSession::onBinaryMessage(const QByteArray &message)
{
    m_batch.clear();
    parsePackets(message, 0);
    if (!m_batch.isEmpty()) {
        LOG_TRACE("LiveDanmaku") << "一批直播弹幕:" << m_batch.size();
        emit danmakuBatch(m_batch);
    }
}

void LiveDanmakuSession::parsePackets(const QByteArray &data, int depth)
{
    if (depth > 4)
        return;

    int offset = 0;
    while (offset + 16 <= data.size()) {
        const quint32 total = readBe32(data, offset);
        const quint16 headerLength = readBe16(data, offset + 4);
        const quint16 version = readBe16(data, offset + 6);
        const quint32 operation = readBe32(data, offset + 8);

        const int available = data.size() - offset;
        if (total < 16 || total > static_cast<quint32>(available)
            || headerLength < 16 || headerLength > total
            || total > static_cast<quint32>(kMaxPacketBytes)) {
            LOG_WARN("LiveDanmaku") << "非法弹幕包: total=" << total
                                    << " header=" << headerLength
                                    << " available=" << available;
            return;
        }

        const QByteArray body = data.mid(offset + headerLength,
                                         total - headerLength);

        LOG_TRACE("LiveDanmaku") << "弹幕包 ver=" << version
                                 << " op=" << operation
                                 << " body=" << body.size();

        if (version == 2) {
            bool ok = false;
            const QByteArray inflated = inflateZlib(body, &ok);
            if (ok)
                parsePackets(inflated, depth + 1);
            else
                LOG_WARN("LiveDanmaku") << "zlib 解压失败, 包体" << body.size() << "字节";
        } else if (version == 3) {
            LOG_WARN("LiveDanmaku") << "收到 brotli 包（当前只请求 zlib），忽略";
        } else {
            handlePacket(version, operation, body);
        }

        offset += static_cast<int>(total);
    }
}

void LiveDanmakuSession::handlePacket(quint16 /*version*/, quint32 operation,
                                      const QByteArray &body)
{
    if (operation == 8) {
        const QJsonObject reply = QJsonDocument::fromJson(body).object();
        const int code = reply.value(QStringLiteral("code")).toInt(-1);
        if (code == 0) {
            m_state = State::Connected;
            m_reconnectAttempt = 0;
            setConnected(true);
            LOG_INFO("LiveDanmaku") << "弹幕认证成功";
        } else {
            fail(QStringLiteral("弹幕认证失败: code=%1").arg(code));
            closeSocket();
            scheduleReconnect();
        }
        return;
    }

    if (operation == 3) // 心跳回复
        return;

    if (operation == 5)
        handleNotification(body);
}

void LiveDanmakuSession::handleNotification(const QByteArray &body)
{
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    const QString cmd = object.value(QStringLiteral("cmd")).toString();
    LOG_TRACE("LiveDanmaku") << "cmd=" << cmd;

    if (cmd.startsWith(QLatin1String("DANMU_MSG"))) {
        const QJsonArray info = object.value(QStringLiteral("info")).toArray();
        if (info.size() < 2)
            return;
        const QJsonArray meta = info.at(0).toArray();
        if (meta.size() < 4)
            return;

        const QString text = info.at(1).toString().trimmed();
        if (text.isEmpty())
            return;

        const int mode = meta.at(1).toInt(1);
        const int size = meta.at(2).toInt(25);
        const quint32 color =
            static_cast<quint32>(meta.at(3).toInt(0xFFFFFF)) & 0xFFFFFFu;

        QVariantMap item;
        item.insert(QStringLiteral("time"), 0);
        item.insert(QStringLiteral("mode"), mode);
        item.insert(QStringLiteral("size"), size);
        item.insert(QStringLiteral("color"),
                    QStringLiteral("#%1").arg(color, 6, 16, QLatin1Char('0')));
        item.insert(QStringLiteral("text"), text.left(kMaxTextLength));
        m_batch.append(item);
        return;
    }

    // 直播状态变化：有些房间会在这里通知下播
    if (cmd == QLatin1String("ROOM_REAL_TIME_MESSAGE_UPDATE")) {
        const QJsonObject data = object.value(QStringLiteral("data")).toObject();
        if (data.contains(QStringLiteral("live_status"))
            && data.value(QStringLiteral("live_status")).toInt(1) == 0) {
            LOG_INFO("LiveDanmaku") << "直播间已下播";
            stop();
        }
    }
}

// ============================
// 状态与重连
// ============================

void LiveDanmakuSession::setConnected(bool connected)
{
    if (m_connected == connected)
        return;
    m_connected = connected;
    if (connected)
        m_heartbeatTimer.start();
    else
        m_heartbeatTimer.stop();
    emit connectedChanged(connected);
}

void LiveDanmakuSession::scheduleReconnect()
{
    if (!m_active || m_reconnectTimer.isActive())
        return;

    const int exponent = qMin(m_reconnectAttempt, 3);
    const int delay = qMin(kReconnectMaxMs, kReconnectBaseMs * (1 << exponent));
    ++m_reconnectAttempt;
    LOG_INFO("LiveDanmaku") << "将在" << delay << "ms 后重连";
    m_reconnectTimer.start(delay);
}

void LiveDanmakuSession::abortApi()
{
    if (m_reply.isNull())
        return;
    QNetworkReply *reply = m_reply.data();
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void LiveDanmakuSession::closeSocket()
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

void LiveDanmakuSession::fail(const QString &message)
{
    LOG_WARN("LiveDanmaku") << message;
    emit errorOccurred(message);
}
