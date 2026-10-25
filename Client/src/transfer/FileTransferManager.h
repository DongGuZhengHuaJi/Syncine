//
// 点对点文件传输：控制消息 + 分块数据全部走已建立的 WebRTC DataChannel。
//
// 与音频/视频链路完全分开：这里不碰媒体轨，只在 voice 连接的数据通道上收发。
// 传输状态保存在本类（而非 QML 窗口）中，因此关闭窗口不会中断任何任务，
// 重新打开即可看到进度与历史。
//
// 控制消息为文本 JSON，文件块为二进制：前 4 字节是发送方分配的传输 id
// （大端），其后为数据。通道本身可靠且保序，故接收端只需按到达顺序写文件，
// 收到 complete 时再核对 SHA-256。
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

private slots:
    void onDataChannelOpened(const QString &peerId);
    void onDataChannelClosed(const QString &peerId);
    void onDataChannelMessage(const QString &peerId, bool binary,
                              const QByteArray &data);
    void onPeerRemoved(const QString &peerId);
    void onRoomLeft();
    void onPumpTimer();
    void onNotifyTimer();

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
    // 断线续传：发送端请求从某个偏移继续；接收端回报已有字节数
    void handleResume(const QString &peerId, const QJsonObject &message);
    void handleResumeAck(const QString &peerId, const QJsonObject &message);

    // ---- 传输推进 ----
    void startPumping();
    // 发送一条记录本轮预算内的数据块；返回是否真的发了数据
    bool sendNextChunk(const TransferRecordPtr &record, qint64 *budget);
    void finishSending(const TransferRecordPtr &record);
    void verifyIncoming(const TransferRecordPtr &record, const QString &sha256);
    void finishRecord(const TransferRecordPtr &record, TransferState state);
    void failRecord(const TransferRecordPtr &record, const QString &message,
                    bool notifyPeer);

    void updatePending(const QString &peerId);
    QString nicknameFor(const QString &peerId) const;
    // 对端是否仍在成员表里（离线等待重连也算在）
    bool peerStillInRoom(const QString &peerId) const;
    // 数据通道恢复后，按断点前的状态重发 offer / resume / complete
    void reissueAfterReconnect(const QString &peerId);
    static QString downloadDirectory();
    static QString sanitizeFileName(const QString &name);
    static QString uniquePath(const QString &directory, const QString &name);

    WebrtcManager *m_webrtc = nullptr;
    RoomSession *m_session = nullptr;

    // peerId → 记录。模型归本类所有，析构时随之释放
    QHash<QString, FileTransferModel *> m_models;
    QSet<QString> m_openChannels;
    // 数据通道发送缓冲达到高水位后暂停该对端的发送，低于低水位再恢复
    QHash<QString, bool> m_sendPaused;
    // 发送队列满时暂存的控制消息，由发送节拍重试（取消/校验消息不能丢）
    QHash<QString, QVector<QByteArray>> m_pendingControl;

    QTimer m_pumpTimer;    // 发送节拍（只有存在发送任务时运行）
    QTimer m_notifyTimer;  // 进度/速度刷新节拍
    quint32 m_nextId = 1;
};

#endif //SYNCINE_FILETRANSFERMANAGER_H
