//
// 房间相关的传输层数据类型。
//


#ifndef SYNCINE_ROOMTYPES_H
#define SYNCINE_ROOMTYPES_H

#include <QList>
#include <QString>

// 房间成员的一次快照(服务端 members[] / member_joined 里的元素)
struct Member {
    QString clientId;
    QString nickname;
    bool isHost = false;
    bool loaded = false;
    qint64 duration = 0;
};

// 播放列表里的一条(服务端 playlist[] 里的元素)。
//
// **没有本地文件路径** —— 路径是每个人自己的事,由客户端的
// PlaylistModel 另外维护一份 itemId → 本地文件 的映射。
// 这里只有"大家共同认可的那份条目定义"。
struct PlaylistEntry {
    QString itemId;
    QString title;
    QString url;        // 可选,网链模式的源
    qint64 duration = 0; // 0 = 还没人加载过,时长未知
    QString addedBy;

    // 这一条的匹配状态,**由服务端聚合**(它才知道所有成员的情况):
    //   "missing"  还有人没有这一条 → 未匹配
    //   "matched"  全员都有且内容一致 → 已匹配
    //   "mismatch" 全员都有但内容不一致 → 不同步
    // 共享/网链模式不含这个字段(那里不存在"匹配"这回事)
    QString status;
};

// 入场时服务端下发的完整房间状态(room_created / room_joined)
struct RoomSnapshot {
    QString roomId;
    QString roomName;
    QString mode;               // 线路格式:"local" / "share" / "url"
    QString clientId;           // 自己的 clientId(来自 welcome 握手)
    QList<Member> members;
    bool hasState = false;      // 服务端是否下发了 state(没有就别去对齐播放器)
    bool playing = false;       // state.playing
    qint64 position = 0;        // state.position
    bool videoMismatched = false;
    qint64 shortestDuration = 0;

    // 入房时服务端一并下发的播放列表(队列 + 现在放到第几条)
    QList<PlaylistEntry> playlist;
    int currentIndex = -1;
};

#endif //SYNCINE_ROOMTYPES_H
