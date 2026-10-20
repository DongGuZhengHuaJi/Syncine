//
// B 站直播弹幕会话：room_init → getConf → WebSocket 认证/心跳/重连 → DANMU_MSG。
//
// 与点播弹幕完全不同的数据模型：没有 XML、没有时间轴、没有游标。
// 消息一到就解析成 QVariantMap 批次，交给 QML 立即生成普通文字弹幕。
//
// 协议（实测于 2026-10）：
//   1. GET room/v1/Room/room_init?id=<短号> → data.room_id（真实房间号）
//   2. GET room/v1/Danmu/getConf?room_id=<真实号>&platform=pc&player=web
//        → data.token + data.host_server_list[]
//   3. wss://<host>:<wss_port>/sub，发 op=7 认证包
//        {"uid":0,"roomid":<真实号>,"protover":2,"platform":"web","type":2,"key":"<token>"}
// 4. 每 30s 发 op=2 心跳（包体 "[object Object]"）
// 5. op=5 的 JSON 里 cmd 以 DANMU_MSG 开头
//
// **登录态**：纯匿名连接会被 B 站明显限流。带 Cookie 时还要把 auth 包的
// uid 填成 Cookie 里的 DedeUserID，并让 getConf 与 WebSocket 都带上同一份
// Cookie；否则 uid 与 token 不匹配会被直接断开。
//
// 包格式：16 字节头（大端）
//   0  4B 整包长度 | 4  2B 头长度(16) | 6  2B 协议版本 | 8  4B 操作码 | 12 4B 序号
//   版本 0/1 = 明文 JSON，2 = zlib，3 = brotli
// 一个二进制帧里可能有多个包；zlib 解压后还可能嵌套多个包，需要递归解析。
// 实测即使认证时写 protover=0，服务器仍会发 ver=2 的 zlib 包，所以 zlib 是硬依赖。
//

#ifndef SYNCINE_LIVEDANMAKUSESSION_H
#define SYNCINE_LIVEDANMAKUSESSION_H

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkCookieJar;
class QNetworkReply;
class QWebSocket;

class LiveDanmakuSession : public QObject {
    Q_OBJECT

public:
    // cookieFilePath 用于拉取最近 20 条历史弹幕（匿名时接口返回空）；
    // 可以为空，不影响实时 WebSocket
    explicit LiveDanmakuSession(const QString &cookieFilePath = QString(),
                                QObject *parent = nullptr);
    ~LiveDanmakuSession() override;

    // pageUrl 用 yt-dlp 探测出的 canonical webpage_url（短链已被解析）
    void start(const QString &pageUrl);
    void stop();

    bool running() const { return m_active; }
    bool connected() const { return m_connected; }

signals:
    // 一批新弹幕；每个元素是 {time, mode, size, color, text}
    void danmakuBatch(const QVariantList &items);
    void connectedChanged(bool connected);
    // 主动停止 / 直播结束 / 无法解析；QML 据此清掉直播弹幕
    void stopped();
    void errorOccurred(const QString &message);

private:
    enum class State {
        Idle,
        ResolvingRoom,
        FetchingConf,
        Connecting,
        Connected,
    };

    void resolveRoomId();
    void fetchConf();
    void openSocket();
    void sendAuth();
    void sendHeartbeat();
    void sendPacket(quint32 operation, const QByteArray &body, quint16 version = 1);

    void onBinaryMessage(const QByteArray &message);
    void parsePackets(const QByteArray &data, int depth);
    void handlePacket(quint16 version, quint32 operation, const QByteArray &body);
    void handleNotification(const QByteArray &body);

    void scheduleReconnect();
    void abortApi();
    void closeSocket();
    void setConnected(bool connected);
    void fail(const QString &message);
    int loadCookies();
    QByteArray cookieFileHeader() const;
    QNetworkAccessManager *apiNetwork() const;

    QNetworkAccessManager *m_network = nullptr;
    // 带 cookie jar 的 API 网络：登录态下 room_init/getConf 用它，
    // 这样拿到的 token 与 Cookie 账号绑定；没有 Cookie 时退回匿名网络。
    QNetworkAccessManager *m_authNetwork = nullptr;
    QNetworkCookieJar *m_cookieJar = nullptr;
    QPointer<QNetworkReply> m_reply;
    QWebSocket *m_socket = nullptr;

    QTimer m_heartbeatTimer;
    QTimer m_reconnectTimer;

    QString m_pageUrl;
    QString m_shortRoomId;
    QString m_roomId;
    QString m_token;
    QStringList m_hosts;
    int m_hostIndex = 0;
    QString m_cookiePath;
    int m_cookieCount = 0;
    // Cookie 里的 DedeUserID；必须与登录态 token 配套，0 = 匿名
    qint64 m_uid = 0;

    State m_state = State::Idle;
    bool m_active = false;
    bool m_connected = false;

    // 会话代次：start/stop 时递增，API 回调据此丢弃旧结果
    quint64 m_generation = 0;
    // 连接代次：每次 openSocket/closeSocket 递增，旧 socket 的回调据此丢弃
    quint64 m_connectionGeneration = 0;
    quint32 m_sequence = 0;
    int m_reconnectAttempt = 0;

    // 当前 WebSocket 帧解析出的弹幕，帧结束时一次性发出去
    QVariantList m_batch;
};

#endif //SYNCINE_LIVEDANMAKUSESSION_H
