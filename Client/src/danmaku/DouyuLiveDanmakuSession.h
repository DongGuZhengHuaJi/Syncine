//
// 斗鱼直播弹幕会话：页面取真实 room_id → danmuproxy WebSocket → chatmsg。
//
// 与 B 站直播弹幕完全不同的协议：
//   · 12 字节小端头：remaining(总长-4) 重复两次，然后 type=689；
//     body = UTF-8 文本 + '\0'；
//   · 登录 type@=loginreq/roomid@={真实房间号}/；
//   · 加群 type@=joingroup/rid@={真实房间号}/gid@=1/；
//   · 心跳 type@=mrkl/（收到 pingreq 立即回一次）；
//   · 聊天 type@=chatmsg/.../nn@=昵称/txt@=内容/；
//     转义 @A -> '@'，@S -> '/'。
//
// 斗鱼房间页是 Next.js，短号（如 410804）不是真实房间号；优先使用插件
// MediaInfo.id 给出的真实号，为空时从页面 window.room_id 解析。
//
// 读聊天不需要登录、Cookie 或签名；实测 danmuproxy:8501 匿名即可。
//
#ifndef SYNCINE_DOUYULIVEDANMAKUSESSION_H
#define SYNCINE_DOUYULIVEDANMAKUSESSION_H

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;
class QWebSocket;

class DouyuLiveDanmakuSession : public QObject {
    Q_OBJECT

public:
    explicit DouyuLiveDanmakuSession(QObject *parent = nullptr);
    ~DouyuLiveDanmakuSession() override;

    // roomIdHint：插件已解析出的真实房间号；为空或非数字时从页面解析。
    void start(const QString &pageUrl, const QString &roomIdHint = QString());
    void stop();

    bool running() const { return m_active; }
    bool connected() const { return m_connected; }

signals:
    // 一批新弹幕；每个元素是 {time, mode, size, color, text}
    void danmakuBatch(const QVariantList &items);
    void connectedChanged(bool connected);
    void stopped();
    void errorOccurred(const QString &message);

private:
    enum class State {
        Idle,
        ResolvingRoom,
        Connecting,
        Connected,
    };

    void resolveRoomId();
    void openSocket();
    void sendLogin();
    void sendJoin();
    void sendHeartbeat();
    void sendPacket(const QString &body);

    void onBinaryMessage(const QByteArray &message);
    void handleMessage(const QString &message);

    void scheduleReconnect();
    void abortNetwork();
    void closeSocket();
    void setConnected(bool connected);
    void fail(const QString &message);

    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    QWebSocket *m_socket = nullptr;

    QTimer m_heartbeatTimer;
    QTimer m_reconnectTimer;

    QString m_pageUrl;
    QString m_roomId;

    State m_state = State::Idle;
    bool m_active = false;
    bool m_connected = false;

    // 会话代次：start/stop 时递增，旧回调据此丢弃
    quint64 m_generation = 0;
    // 连接代次：每次 openSocket/closeSocket 递增
    quint64 m_connectionGeneration = 0;
    int m_reconnectAttempt = 0;
    int m_portIndex = 0;

    QVariantList m_batch;
};

#endif //SYNCINE_DOUYULIVEDANMAKUSESSION_H
