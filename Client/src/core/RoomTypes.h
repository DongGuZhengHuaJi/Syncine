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
};

#endif //SYNCINE_ROOMTYPES_H
