#include "FileTransferModel.h"

#include <utility>

namespace {

constexpr int kMaxRecords = 200; // 单个对端保留的历史条数

bool isActiveState(TransferState state)
{
    return state == TransferState::WaitingAccept
           || state == TransferState::Transferring
           || state == TransferState::Verifying;
}

QString stateId(TransferState state)
{
    switch (state) {
    case TransferState::WaitingAccept:
        return QStringLiteral("waiting");
    case TransferState::Transferring:
        return QStringLiteral("transferring");
    case TransferState::Verifying:
        return QStringLiteral("verifying");
    case TransferState::Completed:
        return QStringLiteral("completed");
    case TransferState::Rejected:
        return QStringLiteral("rejected");
    case TransferState::Cancelled:
        return QStringLiteral("cancelled");
    case TransferState::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("failed");
}

QString stateText(const TransferRecord &record)
{
    switch (record.state) {
    case TransferState::WaitingAccept:
        return record.direction == TransferDirection::Receive
                   ? QStringLiteral("等待你接收")
                   : QStringLiteral("等待对方接收");
    case TransferState::Transferring:
        return record.direction == TransferDirection::Receive
                   ? QStringLiteral("接收中")
                   : QStringLiteral("发送中");
    case TransferState::Verifying:
        return QStringLiteral("校验中…");
    case TransferState::Completed:
        return record.verified ? QStringLiteral("已完成 · 校验通过")
                               : QStringLiteral("已完成");
    case TransferState::Rejected:
        return QStringLiteral("对方已拒绝");
    case TransferState::Cancelled:
        return QStringLiteral("已取消");
    case TransferState::Failed:
        return QStringLiteral("失败");
    }
    return {};
}

QString formatBytes(qint64 bytes)
{
    if (bytes < 0)
        return {};
    if (bytes >= qint64(1024) * 1024 * 1024)
        return QStringLiteral("%1 GB").arg(bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 2);
    if (bytes >= 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / 1024.0 / 1024.0, 0, 'f', 1);
    if (bytes >= 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(bytes);
}

QString formatSpeed(qint64 bytesPerSecond)
{
    if (bytesPerSecond <= 0)
        return {};
    if (bytesPerSecond >= 1024 * 1024)
        return QStringLiteral("%1 MB/s").arg(bytesPerSecond / 1024.0 / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 KB/s").arg(bytesPerSecond / 1024.0, 0, 'f', 0);
}

} // namespace

FileTransferModel::FileTransferModel(QString peerId, QObject *parent)
    : QAbstractListModel(parent),
      m_peerId(std::move(peerId)) {
}

// ============================
// QAbstractListModel
// ============================

int FileTransferModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_records.size());
}

QVariant FileTransferModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_records.size())
        return {};

    const TransferRecord &record = *m_records.at(index.row());
    const bool active = isActiveState(record.state);
    const bool receiving = record.direction == TransferDirection::Receive;

    switch (role) {
    case IdRole:
        return record.id;
    case PeerIdRole:
        return record.peerId;
    case FileNameRole:
        return record.fileName;
    case FileSizeRole:
        return record.totalBytes;
    case TransferredRole:
        return record.transferred;
    case ProgressRole: {
        if (record.state == TransferState::Completed)
            return 100;
        if (record.totalBytes <= 0)
            return 0;
        const qint64 percent = record.transferred * 100 / record.totalBytes;
        return static_cast<int>(qBound<qint64>(qint64(0), percent, qint64(100)));
    }
    case StateRole:
        return stateId(record.state);
    case StateTextRole:
        return stateText(record);
    case DirectionRole:
        return receiving ? QStringLiteral("receive") : QStringLiteral("send");
    case DirectionTextRole:
        return receiving ? QStringLiteral("接收") : QStringLiteral("发送");
    case SpeedTextRole:
        return record.state == TransferState::Transferring
                   ? formatSpeed(record.speedBps)
                   : QString();
    case SizeTextRole:
        return formatBytes(record.totalBytes);
    case TransferredTextRole:
        return formatBytes(record.transferred);
    case ErrorRole:
        return record.error;
    case TargetPathRole:
        return record.targetPath;
    case VerifiedRole:
        return record.verified;
    case ActiveRole:
        return active;
    case AwaitingAcceptRole:
        return record.state == TransferState::WaitingAccept;
    case CanAcceptRole:
        return receiving && record.state == TransferState::WaitingAccept;
    case CanRejectRole:
        return receiving && record.state == TransferState::WaitingAccept;
    case CanCancelRole:
        if (!active)
            return false;
        // 接收方在「待接收」时该点的是拒绝，不是取消
        return !(receiving && record.state == TransferState::WaitingAccept);
    case TimeTextRole:
        return record.createdAt.toString(QStringLiteral("MM-dd HH:mm"));
    default:
        return {};
    }
}

QHash<int, QByteArray> FileTransferModel::roleNames() const {
    return {
        {IdRole, "transferId"},
        {PeerIdRole, "peerId"},
        {FileNameRole, "fileName"},
        {FileSizeRole, "fileSize"},
        {TransferredRole, "transferred"},
        {ProgressRole, "progress"},
        // 不能叫 "state"：delegate 的 Item 自身就有 state 属性，会遮蔽角色
        {StateRole, "transferState"},
        {StateTextRole, "stateText"},
        {DirectionRole, "direction"},
        {DirectionTextRole, "directionText"},
        {SpeedTextRole, "speedText"},
        {SizeTextRole, "sizeText"},
        {TransferredTextRole, "transferredText"},
        // 同理避开与窗口自身属性的重名
        {ErrorRole, "transferError"},
        {TargetPathRole, "targetPath"},
        {VerifiedRole, "verified"},
        {ActiveRole, "active"},
        {AwaitingAcceptRole, "awaitingAccept"},
        {CanAcceptRole, "canAccept"},
        {CanRejectRole, "canReject"},
        {CanCancelRole, "canCancel"},
        {TimeTextRole, "timeText"},
    };
}

// ============================
// 变更
// ============================

void FileTransferModel::addRecord(const TransferRecordPtr &record) {
    if (record == nullptr)
        return;

    // 历史有上限：只淘汰最旧的已完成记录，进行中的一条都不动
    if (m_records.size() >= kMaxRecords) {
        for (int i = m_records.size() - 1; i >= 0; --i) {
            if (isActiveState(m_records.at(i)->state))
                continue;
            beginRemoveRows(QModelIndex(), i, i);
            m_records.removeAt(i);
            endRemoveRows();
            break;
        }
    }

    beginInsertRows(QModelIndex(), 0, 0);
    m_records.prepend(record);
    endInsertRows();
    emit countChanged();
}

void FileTransferModel::updateRecord(const TransferRecordPtr &record) {
    const int row = static_cast<int>(m_records.indexOf(record));
    if (row < 0)
        return;

    emit dataChanged(index(row), index(row));
    // 活动条数可能随状态变化（完成 / 失败），标题上的计数要跟着更新
    emit countChanged();
}

void FileTransferModel::clear() {
    if (m_records.isEmpty())
        return;

    beginResetModel();
    m_records.clear();
    endResetModel();
    emit countChanged();
}

// ============================
// 只读
// ============================

int FileTransferModel::count() const {
    return static_cast<int>(m_records.size());
}

int FileTransferModel::activeCount() const {
    int count = 0;
    for (const TransferRecordPtr &record : m_records) {
        if (isActiveState(record->state))
            ++count;
    }
    return count;
}

int FileTransferModel::pendingIncoming() const {
    int count = 0;
    for (const TransferRecordPtr &record : m_records) {
        if (record->direction == TransferDirection::Receive
            && record->state == TransferState::WaitingAccept) {
            ++count;
        }
    }
    return count;
}
