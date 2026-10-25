//
// 单个对端的文件传输记录（发送 + 接收，进行中的与历史）。
//
// 记录本身由本类持有：窗口关掉后记录仍在，界面重新打开即可看到全部状态。
// 真正的传输逻辑在 FileTransferManager，本类只负责数据与展示字段，
// QML 通过角色名取值（fileName / progress / stateText / ...）。
//

#ifndef SYNCINE_FILETRANSFERMODEL_H
#define SYNCINE_FILETRANSFERMODEL_H

#include <memory>

#include <QAbstractListModel>
#include <QDateTime>
#include <QList>
#include <QString>

#include <qqmlintegration.h>

class QFile;
class QCryptographicHash;

enum class TransferDirection {
    Send,
    Receive,
};

enum class TransferState {
    WaitingAccept,  // 发送方等对方点「接收」
    Transferring,   // 正在传
    Verifying,      // 数据发完，双方核对 SHA-256
    Interrupted,    // 连接中断，等待续传（文件/哈希保留）
    Completed,
    Rejected,
    Cancelled,
    Failed,
};

// 还在进行中（或等待恢复）的传输。终态不在此列。
inline bool isActiveTransferState(TransferState state)
{
    return state == TransferState::WaitingAccept
           || state == TransferState::Transferring
           || state == TransferState::Verifying
           || state == TransferState::Interrupted;
}

struct TransferRecord {
    quint32 id = 0;                  // 发送方分配，二进制块靠它路由
    QString peerId;
    QString peerName;

    TransferDirection direction = TransferDirection::Send;
    TransferState state = TransferState::WaitingAccept;

    QString fileName;
    QString sourcePath;              // 发送：本机源文件
    QString targetPath;              // 接收：最终保存路径
    QString partialPath;             // 接收：传输中的 .part

    qint64 totalBytes = 0;
    qint64 transferred = 0;
    qint64 speedBps = 0;
    QString error;
    bool verified = false;

    QDateTime createdAt;
    QDateTime finishedAt;

    // 运行时资源。传输结束即释放；记录本身留在历史里
    std::shared_ptr<QFile> file;
    std::shared_ptr<QCryptographicHash> hash;

    // 速度采样
    qint64 lastSampleBytes = 0;
    qint64 lastSampleMs = 0;

    // ---- 断线续传 ----
    // 进入 Interrupted 前的状态，恢复时据此决定重发 offer / resume / complete
    TransferState interruptedFrom = TransferState::Failed;
    qint64 interruptedAtMs = 0;
    // 发送端续传时正在重算前缀哈希的剩余字节
    qint64 hashReplayRemaining = 0;
    // 已发出 resume，等待 resume_ack
    bool resumeRequested = false;
    // 发送完成时的 SHA-256，用于断线后重发 complete
    QString finalHash;
};

using TransferRecordPtr = std::shared_ptr<TransferRecord>;

class FileTransferModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("由 FileTransferManager 内部创建")

    Q_PROPERTY(int count
               READ count
               NOTIFY countChanged)

    // 进行中（含待对方接收）的条数，窗口标题可用
    Q_PROPERTY(int activeCount
               READ activeCount
               NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        PeerIdRole,
        FileNameRole,
        FileSizeRole,
        TransferredRole,
        ProgressRole,
        StateRole,
        StateTextRole,
        DirectionRole,
        DirectionTextRole,
        SpeedTextRole,
        SizeTextRole,
        TransferredTextRole,
        ErrorRole,
        TargetPathRole,
        VerifiedRole,
        ActiveRole,
        AwaitingAcceptRole,
        CanAcceptRole,
        CanRejectRole,
        CanCancelRole,
        TimeTextRole,
    };
    Q_ENUM(Role)

    explicit FileTransferModel(QString peerId, QObject *parent = nullptr);

    // ---- QAbstractListModel ----
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // ---- 供 FileTransferManager 使用 ----
    const QList<TransferRecordPtr> &records() const { return m_records; }
    // 新记录插到最前（新消息在上，历史沉底）
    void addRecord(const TransferRecordPtr &record);
    void updateRecord(const TransferRecordPtr &record);
    void clear();

    int count() const;
    int activeCount() const;
    // 待本端接收的条数（成员列表红点）
    int pendingIncoming() const;

signals:
    void countChanged();

private:
    QString m_peerId;
    QList<TransferRecordPtr> m_records;
};

#endif //SYNCINE_FILETRANSFERMODEL_H
