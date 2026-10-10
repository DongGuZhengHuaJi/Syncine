#include "PlaylistModel.h"

PlaylistModel::PlaylistModel(QObject *parent)
    : QAbstractListModel(parent) {
}

// ============================
// QAbstractListModel
// ============================

int PlaylistModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_entries.size());
}

QVariant PlaylistModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size())
        return {};

    const PlaylistEntry &entry = m_entries.at(index.row());

    switch (role) {
    case ItemIdRole:
        return entry.itemId;
    case TitleRole:
        return entry.title;
    case UrlRole:
        return entry.url;
    case DurationRole:
        return entry.duration;
    case AddedByRole:
        return entry.addedBy;
    case IsCurrentRole:
        return index.row() == m_currentIndex;
    case HasLocalFileRole:
        return m_localFiles.contains(entry.itemId);
    case StatusRole:
        return entry.status;
    default:
        return {};
    }
}

QHash<int, QByteArray> PlaylistModel::roleNames() const {
    return {
        {ItemIdRole, "itemId"},
        {TitleRole, "title"},
        {UrlRole, "url"},
        {DurationRole, "duration"},
        {AddedByRole, "addedBy"},
        {IsCurrentRole, "isCurrent"},
        {HasLocalFileRole, "hasLocalFile"},
        {StatusRole, "status"},
    };
}

// ============================
// 变更
// ============================

void PlaylistModel::reset(const QList<PlaylistEntry> &entries, int currentIndex) {
    beginResetModel();
    m_entries = entries;

    int normalized = currentIndex;
    if (normalized < 0 || normalized >= m_entries.size())
        normalized = m_entries.isEmpty() ? -1 : 0;
    const bool currentDidChange = normalized != m_currentIndex;
    m_currentIndex = normalized;
    endResetModel();

    emit countChanged();
    if (currentDidChange)
        emit currentChanged();
}

void PlaylistModel::setEntries(const QList<PlaylistEntry> &entries, int currentIndex) {
    // 服务端不知道时长(它不解析媒体文件),时长是本地加载出来后回报的。
    // 整体替换前先把已知的时长接过来,否则别人加一条新条目,
    // 正在显示的那几条时长会一起变回 "--:--"
    QList<PlaylistEntry> merged = entries;
    for (PlaylistEntry &entry : merged) {
        if (entry.duration > 0)
            continue;
        const int row = indexOf(entry.itemId);
        if (row >= 0)
            entry.duration = m_entries.at(row).duration;
    }

    // 条目数很少(几十条),整体替换比增量 diff 简单得多,也不会算错
    beginResetModel();
    m_entries = merged;

    int normalized = currentIndex;
    if (normalized < 0 || normalized >= m_entries.size())
        normalized = m_entries.isEmpty() ? -1 : 0;
    const bool currentDidChange = normalized != m_currentIndex;
    m_currentIndex = normalized;
    endResetModel();

    emit countChanged();
    if (currentDidChange)
        emit currentChanged();
}

void PlaylistModel::setCurrentIndex(int index) {
    int normalized = index;
    if (normalized < 0 || normalized >= m_entries.size())
        normalized = -1;
    if (normalized == m_currentIndex)
        return;

    const int previous = m_currentIndex;
    m_currentIndex = normalized;

    // isCurrent 影响的是两行(旧的当前、新的当前)
    if (previous >= 0)
        emit dataChanged(this->index(previous), this->index(previous), {IsCurrentRole});
    if (m_currentIndex >= 0)
        emit dataChanged(this->index(m_currentIndex), this->index(m_currentIndex),
                         {IsCurrentRole});

    emit currentChanged();
}

bool PlaylistModel::setDuration(const QString &itemId, qint64 duration) {
    const int row = indexOf(itemId);
    if (row < 0 || duration <= 0 || m_entries[row].duration == duration)
        return false;

    m_entries[row].duration = duration;
    emit dataChanged(index(row), index(row), {DurationRole});
    return true;
}

void PlaylistModel::applyStatuses(const QList<PlaylistEntry> &statuses) {
    for (const PlaylistEntry &update : statuses) {
        const int row = indexOf(update.itemId);
        if (row < 0 || m_entries.at(row).status == update.status)
            continue;

        m_entries[row].status = update.status;
        emit dataChanged(index(row), index(row), {StatusRole});
    }
}

void PlaylistModel::setLocalFile(const QString &itemId, const QString &path) {
    if (itemId.isEmpty())
        return;

    m_localFiles.insert(itemId, path);

    const int row = indexOf(itemId);
    if (row >= 0)
        emit dataChanged(index(row), index(row), {HasLocalFileRole});
    if (itemId == currentItemId())
        emit currentChanged();
}

void PlaylistModel::clear() {
    beginResetModel();
    m_entries.clear();
    m_localFiles.clear(); // 映射是跟着房间里的条目走的,离房就作废
    m_currentIndex = -1;
    endResetModel();

    emit countChanged();
    emit currentChanged();
}

// ============================
// 只读
// ============================

int PlaylistModel::count() const {
    return static_cast<int>(m_entries.size());
}

const QList<PlaylistEntry> &PlaylistModel::entries() const {
    return m_entries;
}

int PlaylistModel::currentIndex() const {
    return m_currentIndex;
}

QString PlaylistModel::currentItemId() const {
    if (m_currentIndex < 0 || m_currentIndex >= m_entries.size())
        return {};
    return m_entries.at(m_currentIndex).itemId;
}

bool PlaylistModel::currentReady() const {
    const QString itemId = currentItemId();
    return !itemId.isEmpty() && m_localFiles.contains(itemId);
}

QString PlaylistModel::localFile(const QString &itemId) const {
    return m_localFiles.value(itemId);
}

bool PlaylistModel::contains(const QString &itemId) const {
    return indexOf(itemId) >= 0;
}

int PlaylistModel::indexOf(const QString &itemId) const {
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries.at(i).itemId == itemId)
            return i;
    }
    return -1;
}
