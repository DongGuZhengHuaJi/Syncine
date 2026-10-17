//
// Created by donggu on 2026/9/13.
//

#include "MemberModel.h"

#include "core/Log.h"

MemberModel::MemberModel(QObject *parent)
    : QAbstractListModel(parent) {
}

// ============================
// QAbstractListModel
// ============================

int MemberModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_members.size());
}

QVariant MemberModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_members.size())
        return {};

    const Member &member = m_members.at(index.row());
    switch (role) {
    case ClientIdRole:
        return member.clientId;
    case NicknameRole:
        return member.nickname;
    case IsHostRole:
        return member.isHost;
    case LoadedRole:
        return member.loaded;
    case DurationRole:
        return member.duration;
    case PendingFilesRole:
        return m_pendingFiles.value(member.clientId, 0);
    default:
        return {};
    }
}

QHash<int, QByteArray> MemberModel::roleNames() const {
    return {
        {ClientIdRole, "clientId"},
        {NicknameRole, "nickname"},
        {IsHostRole, "isHost"},
        {LoadedRole, "loaded"},
        {DurationRole, "duration"},
        {PendingFilesRole, "pendingFiles"},
    };
}

// ============================
// 变更
// ============================

void MemberModel::reset(const QList<Member> &members) {
    LOG_DEBUG("Member") << "重置成员表: 数量=" << members.size();
    beginResetModel();
    m_members = members;
    // 名单之外残留的传输计数一并丢掉（跨房间时不带过去）
    const QList<QString> pendingIds = m_pendingFiles.keys();
    for (const QString &id : pendingIds) {
        if (!contains(id))
            m_pendingFiles.remove(id);
    }
    endResetModel();
    notifyChanged();
}

bool MemberModel::append(const Member &member) {
    if (member.clientId.isEmpty()) {
        LOG_WARN("Member") << "成员缺少 clientId，忽略";
        return false;
    }
    if (contains(member.clientId)) {
        LOG_DEBUG("Member") << "成员已存在，忽略: clientId=" << member.clientId;
        return false;
    }

    beginInsertRows(QModelIndex(), m_members.size(), m_members.size());
    m_members.append(member);
    endInsertRows();
    notifyChanged();
    return true;
}

std::optional<Member> MemberModel::remove(const QString &clientId) {
    for (int i = 0; i < m_members.size(); ++i) {
        if (m_members.at(i).clientId != clientId)
            continue;

        beginRemoveRows(QModelIndex(), i, i);
        const Member removed = m_members.takeAt(i);
        m_pendingFiles.remove(removed.clientId);
        endRemoveRows();
        notifyChanged();
        return removed;
    }
    LOG_DEBUG("Member") << "移除失败，成员不存在: clientId=" << clientId;
    return std::nullopt;
}

bool MemberModel::setVideoStatus(const QString &clientId, bool loaded, qint64 duration) {
    for (int i = 0; i < m_members.size(); ++i) {
        Member &member = m_members[i];
        if (member.clientId != clientId)
            continue;

        if (member.loaded == loaded && member.duration == duration)
            return true;

        member.loaded = loaded;
        member.duration = duration;

        LOG_DEBUG("Member") << "成员视频状态更新: " << member.nickname
                            << " loaded=" << (loaded ? 1 : 0)
                            << " duration=" << duration;

        const QModelIndex changedIndex = index(i);
        emit dataChanged(changedIndex, changedIndex, {LoadedRole, DurationRole});
        notifyChanged();
        return true;
    }

    LOG_DEBUG("Member") << "视频状态更新失败，成员不存在: clientId=" << clientId;
    return false;
}

bool MemberModel::setPendingFiles(const QString &clientId, int count) {
    const int normalized = qMax(0, count);
    if (m_pendingFiles.value(clientId, 0) == normalized)
        return true;

    for (int i = 0; i < m_members.size(); ++i) {
        if (m_members.at(i).clientId != clientId)
            continue;

        if (normalized == 0)
            m_pendingFiles.remove(clientId);
        else
            m_pendingFiles.insert(clientId, normalized);

        emit dataChanged(index(i), index(i), {PendingFilesRole});
        return true;
    }
    return false;
}

void MemberModel::clear() {
    if (m_members.isEmpty())
        return;

    LOG_DEBUG("Member") << "清空成员表: 数量=" << m_members.size();
    beginResetModel();
    m_members.clear();
    m_pendingFiles.clear();
    endResetModel();
    notifyChanged();
}

void MemberModel::notifyChanged() {
    emit countChanged();
    emit changed();
}

// ============================
// 只读访问
// ============================

int MemberModel::count() const {
    return static_cast<int>(m_members.size());
}

const QList<Member> &MemberModel::members() const {
    return m_members;
}

bool MemberModel::contains(const QString &clientId) const {
    for (const Member &member : m_members) {
        if (member.clientId == clientId)
            return true;
    }
    return false;
}

// ============================
// 派生查询
// ============================

bool MemberModel::isHost(const QString &clientId) const {
    for (const Member &member : m_members) {
        if (member.clientId == clientId)
            return member.isHost;
    }
    return false;
}

bool MemberModel::allOthersLoaded(const QString &clientId) const {
    for (const Member &member : m_members) {
        if (member.clientId == clientId)
            continue;
        if (!member.loaded)
            return false;
    }
    return true;
}

QString MemberModel::firstIdExcept(const QString &clientId) const {
    for (const Member &member : m_members) {
        if (member.clientId != clientId)
            return member.clientId;
    }
    return QString();
}

QList<QString> MemberModel::idsExcept(const QString &clientId) const {
    QList<QString> ids;
    ids.reserve(m_members.size());
    for (const Member &member : m_members) {
        if (member.clientId != clientId)
            ids.append(member.clientId);
    }
    return ids;
}
