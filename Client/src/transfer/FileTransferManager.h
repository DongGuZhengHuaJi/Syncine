//
// 点对点文件传输：控制消息与分块数据分别走两条 WebRTC DataChannel。
//
// 与音频/视频链路完全分开：这里不碰媒体轨，只在 voice 连接的数据通道上收发。
// 传输状态保存在本类（而非 QML 窗口）中，因此关闭窗口不会中断任何任务，
// 重新打开即可看到进度与历史。
//
// 两条通道：
//   syncine-file-ctl  reliable + unordered，控制 JSON（offer/accept/ack/...）
//   syncine-file-data reliable + ordered，文件块（前 4 字节为传输 id）
// 控制与文件块分开，取消/校验/心跳不会被大量数据块的重传队列堵住。
//
// 发送端按接收端回报的 ack 推进窗口：未确认的字节数不超过 kSendWindowBytes，
// 避免把几 MB 压进 SCTP 队列。接收端只在超过阈值或间隔时回报一次，合并 ACK。
// 两条通道都有周期性 ping/pong；传输中长时间没有进展会转入 Interrupted 并
// 尝试 resume，而不是无限卡住。
//

#ifndef SYNCINE_FILETRANSFERMANAGER_H
#define SYNCINE_FILETRANSFERMANAGER_H

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include <qqmlintegration.h>

#include "transfer/FileTransferModel.h"

class QJsonObject;
class RoomSession;
class WebrtcManager;

class FileTransferManager : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("由 main 创建并注入")

public:
    FileTransferManager(WebrtcManager *webrtc,
                        RoomSession *session,
                        QObject *parent = nullptr);
    ~FileTransferManager() override;

    // 某一对端的传输记录（懒创建）。QML 的传输窗口把它当 ListView 的 model
    Q_INVOKABLE FileTransferModel *transfersFor(const QString &peerId);

    // 数据通道是否已打开。未打开时不允许发起传输
    Q_INVOKABLE bool isConnected(const QString &peerId) const;

    // ---- 用户操作（QML 调用）----
    Q_INVOKABLE void sendFile(const QString &peerId, const QUrl &fileUrl);
    Q_INVOKABLE void acceptTransfer(const QString &peerId, quint32 id);
    Q_INVOKABLE void rejectTransfer(const QString &peerId, quint32 id);
    Q_INVOKABLE void cancelTransfer(const QString &peerId, quint32 id);
    // 在文件管理器中打开接收目录
    Q_INVOKABLE void revealTransfer(const QString &peerId, quint32 id);

signals:
    // 收到对方的发送请求。界面据此提示并打开传输窗口
    void incomingOffer(const QString &peerId, const QString &peerName,
                       const QString &fileName);
    void errorOccurred(const QString &peerId, const QString &message);
    // 数据通道的连通状态变化（窗口用来启停发送按钮）
    void connectionChanged(const QString &peerId);
    // 是否有进行中的传输（供共享视频临时降低码率）
    void transferActivityChanged(bool active);

private slots:
    void onDataChannelOpened(const QString &peerId, const QString &channel);
    void onDataChannelClosed(const QString &peerId, const QString &channel);
    void onDataChannelMessage(const QString &peerId, const QString &channel,
                              bool binary, const QByteArray &data);
    void onPeerRemoved(const QString &peerId);
    void onRoomLeft();
    void onPumpTimer();
    void onNotifyTimer();
    void onHealthTimer();

private:
    // ---- 记录查找 ----
    FileTransferModel *ensureModel(const QString &peerId);
    FileTransferModel *modelFor(const QString &peerId) const;
    TransferRecordPtr findOutgoing(const QString &peerId, quint32 id) const;
    TransferRecordPtr findIncoming(const QString &peerId, quint32 id) const;

    // ---- 协议 ----
    bool sendControl(const QString &peerId, const QJsonObject &message);
    void handleOffer(const QString &peerId, const QJsonObject &message);
    void handleAccept(const QString &peerId, const QJsonObject &message);
    void handleReject(const QString &peerId, const QJsonObject &message);
    void handleCancel(const QString &peerId, const QJsonObject &message);
    void handleComplete(const QString &peerId, const QJsonObject &message);
    void handleResult(const QString &peerId, const QJsonObject &message);
    // 接收端回报已连续收到的字节数（应用层 ACK）
    void handleAck(const QString &peerId, const QJsonObject &message);
    // 断线续传：发送端请求从某个偏移继续；接收端回报已有字节数
    void handleResume(const QString &peerId, const QJsonObject &message);
    void handleResumeAck(const QString &peerId, const QJsonObject &message);
    void handlePing(const QString &peerId, const QJsonObject &message);
    void handlePong(const QString &peerId, const QJsonObject &message);

    // ---- 传输推进 ----
    void startPumping();
    // 发送一条记录本轮预算内的数据块；返回是否真的发了数据
    bool sendNextChunk(const TransferRecordPtr &record, qint64 *budget);
    void finishSending(const TransferRecordPtr &record);
    void verifyIncoming(const TransferRecordPtr &record, const QString &sha256);
    void finishRecord(const TransferRecordPtr &record, TransferState state);
    void failRecord(const TransferRecordPtr &record, const QString &message,
                    bool notifyPeer);

    // ---- 可靠性与恢复 ----
    // 发一条 ACK（接收端，合并发送）
    void sendAck(const TransferRecordPtr &record);
    // 进入 Interrupted 并保留文件/哈希，等待 resume 恢复
    void markInterrupted(const TransferRecordPtr &record);
    // 从接收端确认的偏移重新开始发送。allowRewind 只在通道重建后为真：
    // 旧通道上可能还有 SCTP 重传中的数据块，回退重发会造成重复。
    bool resumeFromReceived(const TransferRecordPtr &record, qint64 received,
                            bool allowRewind);
    // 数据停滞时主动发 resume 请求（对端重发 resume_ack）
    void attemptResume(const TransferRecordPtr &record);
    // 两条通道都不可用：中断该对端的进行中传输（对端已离开则直接失败）
    void handleChannelDown(const QString &peerId, const QString &reason);
    // 收到任意控制消息：刷新心跳时间；从“无响应”恢复时重新挂接传输
    void noteChannelAlive(const QString &peerId);

    void updatePending(const QString &peerId);
    QString nicknameFor(const QString &peerId) const;
    // 对端是否仍在成员表里（离线等待重连也算在）
    bool peerStillInRoom(const QString &peerId) const;
    // 数据通道恢复后，按断点前的状态重发 offer / resume / complete。
    // allowRewind 表示通道是否已经重建（新 SCTP 流，旧数据不会再来）。
    void reissueAfterReconnect(const QString &peerId, bool allowRewind);
    static QString downloadDirectory();
    static QString sanitizeFileName(const QString &name);
    static QString uniquePath(const QString &directory, const QString &name);

    // ---- 应用层发送窗口 ----
    // 默认固定 512KB；SYNCINE_TRANSFER_ADAPTIVE=1 时按 ACK 速率在 256KB~1MB
    // 之间 AIMD。**不用 availableOutgoingBitrate**（实测不可信）。
    qint64 sendWindowForPeer(const QString &peerId) const;
    qint64 highWaterForPeer(const QString &peerId) const;
    void noteAdaptiveAck(const QString &peerId, qint64 confirmed);
    void noteAdaptiveStall(const QString &peerId);

    struct AdaptiveWindow {
        qint64 window = 512 * 1024;
        qint64 lastConfirmed = 0;
        qint64 lastUpdateMs = 0;
    };

    WebrtcManager *m_webrtc = nullptr;
    RoomSession *m_session = nullptr;

    // peerId → 记录。模型归本类所有，析构时随之释放
    QHash<QString, FileTransferModel *> m_models;
    // 两条通道各自就绪的对端。只有两条都打开才算真正连通
    QSet<QString> m_controlOpen;
    QSet<QString> m_dataOpen;
    // 心跳超时判定为无响应的对端；收到任意控制消息即恢复
    QSet<QString> m_unresponsive;
    // 最近一次收到控制消息/心跳回应的时间
    QHash<QString, qint64> m_lastPongMs;
    // 数据通道发送缓冲达到高水位后暂停该对端的发送，低于低水位再恢复
    QHash<QString, bool> m_sendPaused;
    // 发送队列满时暂存的控制消息，由发送节拍重试（取消/校验消息不能丢）
    QHash<QString, QVector<QByteArray>> m_pendingControl;

    QTimer m_pumpTimer;    // 发送节拍（只有存在发送任务时运行）
    QTimer m_notifyTimer;  // 进度/速度刷新节拍
    QTimer m_healthTimer;  // 两条数据通道的 ping/pong
    // GetStats 诊断：只在有传输时低频采样，避免空闲时刷日志。
    // m_statsPeers 变化（开始/结束新传输）时立即采一次，之后每 15 秒一次。
    QSet<QString> m_statsPeers;
    qint64 m_lastStatsMs = 0;
    quint32 m_nextId = 1;

    QHash<QString, AdaptiveWindow> m_adaptiveWindows;
    bool m_adaptiveWindowEnabled = false;
    bool m_lastActivity = false;
};

#endif //SYNCINE_FILETRANSFERMANAGER_H
