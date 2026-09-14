//
// Created by donggu on 2026/9/13.
//

#include "MemberModel.h"

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
    };
}

// ============================
// 变更
// ============================

void MemberModel::reset(const QList<Member> &members) {
    beginResetModel();
    m_members = members;
    endResetModel();
    notifyChanged();
}

bool MemberModel::append(const Member &member) {
    if (member.clientId.isEmpty() || contains(member.clientId))
        return false;

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
        endRemoveRows();
        notifyChanged();
        return removed;
    }
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

        const QModelIndex changedIndex = index(i);
        emit dataChanged(changedIndex, changedIndex, {LoadedRole, DurationRole});
        notifyChanged();
        return true;
    }
    return false;
}

void MemberModel::clear() {
    if (m_members.isEmpty())
        return;

    beginResetModel();
    m_members.clear();
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
