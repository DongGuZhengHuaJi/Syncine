
#ifndef SYNCINE_PROTOCOL_H
#define SYNCINE_PROTOCOL_H

#include <QList>
#include <QString>
#include <optional>

#include "RoomTypes.h"

namespace Protocol {

// 服务端 -> 客户端 的消息类型
enum class MessageType {
    Unknown,

    // 会话建立阶段
    Welcome,
    RoomCreated,
    RoomJoined,
    RoomLeft,
    Error,

    // 房间内
    MemberJoined,
    MemberLeft,
    Chat,
    Playback,
    RoomModeChanged,
    VideoStatus,
    VideoMismatch,

    // WebRTC 信令
    WebrtcOffer,
    WebrtcAnswer,
    WebrtcIce,
};

// 解码后的一条服务端消息。
//
// 这里刻意用"扁平结构 + type 判别",而不是 std::variant<每种消息一个 struct>:
// 代价是字段没有按 type 隔离(读错字段会拿到默认值),
// 换来的是调用方一个 switch 就能读完,不需要模板分发。
// 消息种类在 20 以内时这个取舍是划算的。
struct Message {
    MessageType type = MessageType::Unknown;

    // welcome
    QString clientId;

    // room_created / room_joined
    QString roomId;
    QString roomName;
    QString mode;               // 线路格式:"local" / "share" / "url"
    QList<Member> members;
    bool videoMismatched = false;
    qint64 shortestDuration = 0;

    // member_joined / member_left(clientId + 下面这几个字段)
    QString nickname;
    bool isHost = false;
    bool loaded = false;
    qint64 duration = 0;

    // chat
    QString from;
    QString text;

    // playback:action + position
    // room_joined 里 state.playing / state.position 也复用这两个字段
    QString action;
    bool hasState = false;
    bool playing = false;
    qint64 position = 0;

    // webrtc_*
    QString sdp;
    QString sdpMid;
    int sdpMLineIndex = 0;

    // error
    QString code;
    QString errorMessage;
};

// 解析失败(非法 JSON / 没有 type)返回 nullopt
std::optional<Message> decode(const QString &text);

// ---- 客户端 -> 服务端:组包并序列化成可直接发送的文本 ----

QString encodeCreateRoom(const QString &roomId,
                         const QString &roomName,
                         const QString &nickname,
                         const QString &password);

QString encodeJoinRoom(const QString &roomId,
                       const QString &nickname,
                       const QString &password);

QString encodeLeaveRoom();

QString encodeSetRoomMode(const QString &mode);

QString encodeChat(const QString &text);

QString encodePlayback(const QString &action, qint64 position);

QString encodeVideoStatus(bool loaded, const QString &hash, qint64 duration);

QString encodeWebrtcOffer(const QString &to, const QString &sdp);

QString encodeWebrtcAnswer(const QString &to, const QString &sdp);

QString encodeWebrtcIce(const QString &to,
                        const QString &sdp,
                        const QString &sdpMid,
                        int sdpMLineIndex);

} // namespace Protocol

#endif //SYNCINE_PROTOCOL_H
