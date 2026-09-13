//
// 房间成员表 —— 房间内唯一的成员数据源。
//
// 为什么用 QAbstractListModel 而不是 QVariantList<QVariantMap>:
//   1. 类型安全:取值走 role,字段名写错会立刻暴露,
//      而不是静默拿到一个空 QVariant
//   2. QML 可以直接当 model 用。注意 delegate 里要用**角色名本身**取值:
//
//          Label { text: nickname }        // 对
//          Label { text: modelData.nickname }  // 错,恒为 undefined
//
//      modelData 只在"模型是 JS 数组、或没声明 role 的 ListModel"时才等于
//      当前元素。这里定义了 roleNames,委托的上下文属性就是 nickname /
//      isHost / loaded / clientId 这几个名字,没有 modelData 这个东西。
//      写错的后果是 delegate 整片空白且不报任何错误,非常难查。
//   3. 增删改全部经过这里,派生量只需要在一个地方重算并发信号,
//      不会再出现"改了数据却漏 emit 某个属性"导致 QML 静默不同步
//

#ifndef SYNCINE_MEMBERMODEL_H
#define SYNCINE_MEMBERMODEL_H

#include <QAbstractListModel>
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

public:
    enum Role {
        ClientIdRole = Qt::UserRole + 1,
        NicknameRole,
        IsHostRole,
        LoadedRole,
        DurationRole,
    };
    Q_ENUM(Role)

    explicit MemberModel(QObject *parent = nullptr);

    // ---- QAbstractListModel ----
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // ---- 变更接口 ----
    // 整体替换(入场时服务端下发完整名单)
    void reset(const QList<Member> &members);
    // 追加成员;已存在同名 clientId 则忽略并返回 false
    bool append(const Member &member);
    // 删除成员;返回被删掉的成员(不存在则返回 nullopt)
    std::optional<Member> remove(const QString &clientId);
    // 更新某成员的视频加载状态;返回是否命中
    bool setVideoStatus(const QString &clientId, bool loaded, qint64 duration);
    void clear();

    // ---- 只读访问 ----
    int count() const;
    const QList<Member> &members() const;
    bool contains(const QString &clientId) const;

    // ---- 派生查询 ----
    // 成员数很少(个位数),线性扫描足够,不值得为它维护索引
    bool isHost(const QString &clientId) const;
    // 除 clientId 之外的人是否都已加载视频;房间里只有自己时返回 true
    bool allOthersLoaded(const QString &clientId) const;
    // 房间里第一个非 clientId 的成员(没有则返回空串)
    QString firstIdExcept(const QString &clientId) const;

signals:
    void countChanged();

    // 成员表发生任何变化(增 / 删 / 改)。RoomSession 靠它统一重算派生量,
    // 而不用分别去接 modelReset / rowsInserted / rowsRemoved / dataChanged
    void changed();

private:
    void notifyChanged();

    QList<Member> m_members;
};

#endif //SYNCINE_MEMBERMODEL_H
