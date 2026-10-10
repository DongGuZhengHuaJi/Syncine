//
// 播放列表 —— 房间共享的那份队列,加上"各人自己的本地文件映射"。
//
// 为什么这两件事放在一个 model 里:
//   服务端只知道条目定义(itemId/标题/URL/时长/谁加的),**本地文件路径不上传**
//   —— 本地模式下每个人都要把同一条映射到自己机器上的文件,共享模式下只有
//   房主的映射有意义。界面要显示"这一条我这边能不能放",必须同时看到
//   "房间的条目"和"我的映射",所以由这个 model 把它们合到一起,
//   折算成 hasLocalFile 这个 role 给 QML 用。
//
// 和 MemberModel 一样是 QAbstractListModel:delegate 里用**角色名本身**取值
// (title / isCurrent / hasLocalFile),不要写 model.xxx 或 modelData.xxx。
//

#ifndef SYNCINE_PLAYLISTMODEL_H
#define SYNCINE_PLAYLISTMODEL_H

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QString>

#include <qqmlintegration.h>

#include "core/RoomTypes.h"

class PlaylistModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("由 RoomSession 内部创建")

    Q_PROPERTY(int count
               READ count
               NOTIFY countChanged)

    Q_PROPERTY(QString currentItemId
               READ currentItemId
               NOTIFY currentChanged)

    // 当前条目在本机能不能放(有本地文件映射)。
    // 界面靠它决定"点画面是弹文件夹还是播放/暂停"
    Q_PROPERTY(bool currentReady
               READ currentReady
               NOTIFY currentChanged)

public:
    enum Role {
        ItemIdRole = Qt::UserRole + 1,
        TitleRole,
        UrlRole,
        DurationRole,
        AddedByRole,
        IsCurrentRole,
        HasLocalFileRole,
        StatusRole,      // 服务端聚合的匹配状态,见 PlaylistEntry::status
    };
    Q_ENUM(Role)

    explicit PlaylistModel(QObject *parent = nullptr);

    // ---- QAbstractListModel ----
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // ---- 变更接口 ----
    // 整体替换(入场时服务端下发的 state.playlist)
    void reset(const QList<PlaylistEntry> &entries, int currentIndex);
    // 列表内容变了(playlist_changed):条目增删后服务端发来的权威列表
    void setEntries(const QList<PlaylistEntry> &entries, int currentIndex);
    void setCurrentIndex(int index);
    // 某一条的时长(有人加载完才知道,来自 video_status)
    bool setDuration(const QString &itemId, qint64 duration);
    // 服务端聚合后的匹配状态(items 里只有 itemId + status)
    void applyStatuses(const QList<PlaylistEntry> &statuses);
    // 本地文件映射:记下"这一条对应本机哪个文件"
    void setLocalFile(const QString &itemId, const QString &path);
    void clear();

    // ---- 只读访问 ----
    int count() const;
    const QList<PlaylistEntry> &entries() const;
    int currentIndex() const;
    QString currentItemId() const;
    bool currentReady() const;
    QString localFile(const QString &itemId) const;
    // 条目是否存在
    bool contains(const QString &itemId) const;

signals:
    void countChanged();
    void currentChanged();

private:
    int indexOf(const QString &itemId) const;

    QList<PlaylistEntry> m_entries;
    int m_currentIndex = -1;
    // itemId → 本机文件路径。会话级,不上传也不落盘(第二期再做按内容哈希的持久化)
    QHash<QString, QString> m_localFiles;
};

#endif //SYNCINE_PLAYLISTMODEL_H
