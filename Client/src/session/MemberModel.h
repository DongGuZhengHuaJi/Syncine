//
// 房间成员表：房间内唯一的成员数据源。
//
// 使用 QAbstractListModel 而非 QVariantList<QVariantMap> 的原因：取值走 role，
// 字段名写错会立即暴露而非静默返回空 QVariant；增删改全部经此，派生量只需在一处
// 重算并发出信号，避免漏 emit 导致 QML 静默不同步。
//
// QML 中 delegate 须用**角色名本身**取值（`text: nickname`），
// 不可写 `modelData.nickname`——modelData 仅在模型为 JS 数组或未声明 role 的
// ListModel 时才等于当前元素。此处定义了 roleNames，委托的上下文属性即为
// nickname / isHost / loaded / clientId。写错会导致 delegate 整片空白且不报错。
//

#ifndef SYNCINE_MEMBERMODEL_H
#define SYNCINE_MEMBERMODEL_H

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <optional>

#include <qqmlintegration.h>

#include "core/RoomTypes.h"

class MemberModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("由 RoomSession 内部创建")

    Q_PROPERTY(int count
               READ count
               NOTIFY countChanged)
    Q_PROPERTY(int onlineCount
               READ onlineCount
               NOTIFY changed)

public:
    enum Role {
        ClientIdRole = Qt::UserRole + 1,
        NicknameRole,
        IsHostRole,
        LoadedRole,
        DurationRole,
        // 该成员发来、尚待本端接收的文件数（界面上的红点）
        PendingFilesRole,
        // 断线重连宽限期内为 false
        OnlineRole,
    };
    Q_ENUM(Role)

    explicit MemberModel(QObject *parent = nullptr);

    // ---- QAbstractListModel ----
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // ---- 变更接口 ----
    // 整体替换（入场时服务端下发完整名单）
    void reset(const QList<Member> &members);
    // 追加成员；已存在同名 clientId 则忽略并返回 false
    bool append(const Member &member);
    // 删除成员；返回被删掉的成员（不存在则返回 nullopt）
    std::optional<Member> remove(const QString &clientId);
    // 更新某成员的视频加载状态；返回是否命中
    bool setVideoStatus(const QString &clientId, bool loaded, qint64 duration);
    // 更新在线状态（断线重连恢复时用）
    bool setOnline(const QString &clientId, bool online);
    // 更新「待接收文件数」。文件传输与成员数据分属两层，由 FileTransferManager 写入
    bool setPendingFiles(const QString &clientId, int count);
    void clear();

    // ---- 只读访问 ----
    int count() const;
    int onlineCount() const;
    const QList<Member> &members() const;
    bool contains(const QString &clientId) const;
    // 成员是否在线；成员不存在返回 false
    bool isOnline(const QString &clientId) const;

    // ---- 派生查询 ----
    // 成员数很少（个位数）,线性扫描足够，不值得为它维护索引
    bool isHost(const QString &clientId) const;
    // 除 clientId 之外的人是否都已加载视频；房间里只有自己时返回 true
    bool allOthersLoaded(const QString &clientId) const;
    // 房间里第一个非 clientId 的成员（没有则返回空串）
    QString firstIdExcept(const QString &clientId) const;
    // 除 clientId 之外的全部成员 ID(网状网建连时用：每个对端都要连一条)
    QList<QString> idsExcept(const QString &clientId) const;

signals:
    void countChanged();

    // 成员表发生任何变化（增 / 删 / 改）。RoomSession 靠它统一重算派生量，
    // 而不用分别去接 modelReset / rowsInserted / rowsRemoved / dataChanged
    void changed();

private:
    void notifyChanged();

    QList<Member> m_members;
    // 成员快照（Member）由服务端生成，不该塞入纯本地的传输状态，故单列一份
    QHash<QString, int> m_pendingFiles;
};

#endif //SYNCINE_MEMBERMODEL_H
