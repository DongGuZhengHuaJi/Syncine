//
// 与服务器的 WebSocket 连接。
//
// 这一版的连接策略(设置功能带来的):
//   · 地址来自 AppSettings —— 用户自己的地址优先,**连不上就自动试兜底地址**
//   · 两个都连不上就**定时重试**(15s → 30s → 60s 封顶),服务端晚开一会儿
//     也能自己连上,不用重启客户端
//   · **不再在构造函数里自动连接**。以前是构造即连,结果"读设置"永远赶不上
//     第一次 open(),而且构造期发出的 errorOccurred 一个订阅者都没有。
//     现在由 main() 在装配完成、读完设置之后显式调 connectToServer()。
//
// 两个"失败"信号的分工(别混):
//   errorOccurred    —— **某一次尝试**没成功。诊断用,可能连着发好几次
//   connectionFailed —— **一整轮**(用户地址 + 兜底)都没成功。界面该报错了
//

#ifndef SYNCINE_NETWORKMANAGER_H
#define SYNCINE_NETWORKMANAGER_H

#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <memory>

class AppSettings;

class NetworkManager: public QObject{
    Q_OBJECT

    Q_PROPERTY(bool isConnected
               READ isConnected
               NOTIFY isConnectedChanged)

    // 正在尝试连接(第一次/重试都算)。界面显示"连接中…"
    Q_PROPERTY(bool connecting
               READ connecting
               NOTIFY stateChanged)

    // 已经失败,正在等下一次重试。界面显示"未连接 · 重试中"
    Q_PROPERTY(bool retrying
               READ retrying
               NOTIFY stateChanged)

    // 当前实际在连(或已连上)的地址 —— 用它告诉用户"连的是哪一台"
    Q_PROPERTY(QString effectiveUrl
               READ effectiveUrl
               NOTIFY stateChanged)

    // 连的是不是兜底地址(用户地址是空的、或者试过用户地址没成功)
    Q_PROPERTY(bool usingFallback
               READ usingFallback
               NOTIFY stateChanged)

public:
    // settings 得在构造前就绪(地址来自它)
    explicit NetworkManager(AppSettings *settings, QObject *parent = nullptr);

    bool isConnected() const;
    bool connecting() const;
    bool retrying() const;
    QString effectiveUrl() const;
    bool usingFallback() const;

    // 从头试一轮(用户地址 → 兜底)。**地址改了、或者想手动重连都调它**
    Q_INVOKABLE void connectToServer();
    // 立刻重试,不等重试定时器
    Q_INVOKABLE void retryNow();
    // 用户主动断开:断开之后**不再自动重连**(否则刚断开就自己连回来)
    Q_INVOKABLE void disconnectFromServer();

    // 发送失败(未连接 / 空消息)返回 false 且不发信号,
    // 由调用方决定怎么向上报告 —— 传输层不该猜业务的错误文案
    bool sendMessage(const QString &message);

signals:
    void connected();
    void disconnected();
    void isConnectedChanged();
    // connecting / retrying / effectiveUrl / usingFallback 共用这一个
    void stateChanged();
    void messageReceived(const QString &message);
    void errorOccurred(const QString &errorString);
    void connectionFailed(const QString &reason);

private:
    // 按 AppSettings 里的地址组一轮:用户地址(非空且和兜底不同)+ 兜底
    void startRound();
    void tryNextUrl();
    void startAttempt(const QUrl &url);
    void handleAttemptFailed(const QString &reason);
    void scheduleRetry();
    void stopTimers();
    // 主动掐掉当前连接。abort 也会触发 errorOccurred,用 m_switching 挡掉那一次
    void abortSocket();

    AppSettings *m_settings = nullptr;
    std::unique_ptr<QWebSocket> m_webSocket;

    QTimer m_connectTimer;   // 单次连接的超时
    QTimer m_retryTimer;     // 整轮失败后的等待

    QStringList m_attempts;  // 本轮要依次试的地址
    int m_attemptIndex = 0;
    QUrl m_currentUrl;
    bool m_usingFallback = false;

    bool m_connected = false;      // 真的连上过(区别于"正在连")
    bool m_retrying = false;
    int m_failedRounds = 0;
    bool m_userDisconnected = false;
    bool m_switching = false;      // 自己 abort 中,下一次 error 不是真失败
};


#endif //SYNCINE_NETWORKMANAGER_H
