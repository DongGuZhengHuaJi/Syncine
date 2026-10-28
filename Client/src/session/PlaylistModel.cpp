#include "PlaylistModel.h"

#include "core/Log.h"
#include "media/FormatModel.h"

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
    case LiveRole:
        return entry.live;
    case SpecRole:
        return entry.spec;
    case SpecLabelRole:
        return specLabelOf(entry);
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
        {LiveRole, "live"},
        {SpecRole, "spec"},
        {SpecLabelRole, "specLabel"},
    };
}

// ============================
// 变更
// ============================

void PlaylistModel::reset(const QList<PlaylistEntry> &entries, int currentIndex) {
    const QList<PlaylistEntry> oldEntries = m_entries;
    const QString oldCurrent = currentItemId();

    beginResetModel();
    m_entries = entries;

    int normalized = currentIndex;
    if (normalized < 0 || normalized >= m_entries.size())
        normalized = m_entries.isEmpty() ? -1 : 0;
    const bool currentDidChange = normalized != m_currentIndex;
    m_currentIndex = normalized;
    endResetModel();

    LOG_DEBUG("Playlist") << "重置列表: 条目=" << m_entries.size()
                          << " currentIndex=" << m_currentIndex;
    emit countChanged();
    if (currentDidChange)
        emit currentChanged();
    emitRemovedItems(oldEntries, oldCurrent);
}

void PlaylistModel::setEntries(const QList<PlaylistEntry> &entries, int currentIndex) {
    // 服务端不解析媒体文件，故不知道时长；时长由本地加载后回报。
    // 整体替换前先继承已知时长，否则他人新增一条条目时，
    // 正在显示的其他条目时长会一并退回 "--:--"。
    QList<PlaylistEntry> merged = entries;
    for (PlaylistEntry &entry : merged) {
        if (entry.duration > 0)
            continue;
        const int row = indexOf(entry.itemId);
        if (row >= 0)
            entry.duration = m_entries.at(row).duration;
    }

    const QList<PlaylistEntry> oldEntries = m_entries;
    const QString oldCurrent = currentItemId();

    // 条目数很少（几十条），整体替换比增量 diff 简单得多，也不易算错
    beginResetModel();
    m_entries = merged;

    int normalized = currentIndex;
    if (normalized < 0 || normalized >= m_entries.size())
        normalized = m_entries.isEmpty() ? -1 : 0;
    const bool currentDidChange = normalized != m_currentIndex;
    m_currentIndex = normalized;
    endResetModel();

    LOG_DEBUG("Playlist") << "更新列表: 条目=" << m_entries.size()
                          << " currentIndex=" << m_currentIndex;
    emit countChanged();
    if (currentDidChange)
        emit currentChanged();
    emitRemovedItems(oldEntries, oldCurrent);
}

void PlaylistModel::setCurrentIndex(int index) {
    int normalized = index;
    if (normalized < 0 || normalized >= m_entries.size())
        normalized = -1;
    if (normalized == m_currentIndex)
        return;

    const int previous = m_currentIndex;
    m_currentIndex = normalized;

    LOG_DEBUG("Playlist") << "切换当前条目: index " << previous << " → " << m_currentIndex
                          << " itemId=" << currentItemId();

    // isCurrent 影响两行（原当前项与新当前项）
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
    LOG_DEBUG("Playlist") << "记录条目时长: itemId=" << itemId << " duration=" << duration;
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

    // 空路径 = 撤销该条目的映射（重试时需要此行为：使缓存失效并重新下载）。
    // 不可 insert 空值——否则 hasLocalFile 会报 true（界面显示「就绪」），
    // 但实际没有可播放的文件。
    if (path.isEmpty()) {
        LOG_DEBUG("Playlist") << "清除本地文件映射: itemId=" << itemId;
        m_localFiles.remove(itemId);
    } else {
        LOG_DEBUG("Playlist") << "绑定本地文件: itemId=" << itemId << " 路径=" << path;
        m_localFiles.insert(itemId, path);
    }

    const int row = indexOf(itemId);
    if (row >= 0)
        emit dataChanged(index(row), index(row), {HasLocalFileRole});
    if (itemId == currentItemId())
        emit currentChanged();
}

void PlaylistModel::clear() {
    LOG_DEBUG("Playlist") << "清空列表: 条目=" << m_entries.size()
                          << " 本地映射=" << m_localFiles.size();
    beginResetModel();
    m_entries.clear();
    m_localFiles.clear(); // 映射随房间条目存在，离开房间即作废
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

bool PlaylistModel::currentIsLive() const {
    if (m_currentIndex < 0 || m_currentIndex >= m_entries.size())
        return false;
    return m_entries.at(m_currentIndex).live;
}

bool PlaylistModel::currentReady() const {
    const QString itemId = currentItemId();
    return !itemId.isEmpty() && m_localFiles.contains(itemId);
}

QString PlaylistModel::localFile(const QString &itemId) const {
    return m_localFiles.value(itemId);
}

QString PlaylistModel::specOf(const QString &itemId) const {
    const int row = indexOf(itemId);
    return row >= 0 ? m_entries.at(row).spec : QString();
}

// spec → 人话标签（"1080P" / "手选 720P" / "最高画质"）。
// 手选的 format_id 和竖屏档位都要对着 formats 才翻得准，而列表这边没有
// formats —— 传空 MediaInfo,翻不出来的部分退化成"手选"/原样数字。
// (播放页手上有一整份 formats,那边显示的 label 才是全的)
QString PlaylistModel::specLabelOf(const PlaylistEntry &entry) const
{
    if (entry.spec.isEmpty())
        return entry.live ? QStringLiteral("直播") : QStringLiteral("最高画质");

    // 现在每次进房都会重建模型，逐行解析没必要开缓存；真变热了再考虑
    return StreamSpec::label(entry.spec, MediaInfo());
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

void PlaylistModel::emitRemovedItems(const QList<PlaylistEntry> &oldEntries,
                                     const QString &oldCurrentId) {
    // 只在服务端确认后的新列表上比对；调用时 m_entries 已经是新列表。
    for (const PlaylistEntry &entry : oldEntries) {
        if (contains(entry.itemId))
            continue;

        const bool wasCurrent = entry.itemId == oldCurrentId;
        LOG_INFO("Playlist") << "条目已移除: itemId=" << entry.itemId
                             << " wasCurrent=" << wasCurrent;
        emit itemRemoved(entry.itemId, wasCurrent);
    }
}
