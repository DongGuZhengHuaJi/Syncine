
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
    // 断线重连的成员恢复在线（clientId 不变，成员表里本来就有他）。
    // 不能复用 member_joined：那会让界面显示"加入了房间"并触发重复插入。
    MemberOnline,
    // 成员断线，进入重连宽限（成员表里仍保留，online=false）
    MemberOffline,
    Chat,
    Playback,
    PlaybackPosition,   // 房主周期性广播的播放位置（观众据此绘制进度条）
    RoomModeChanged,
    // 共享模式下的共享源变化（房主广播）：media / screen / window
    ShareState,
    VideoStatus,
    VideoMismatch,
    PlaylistChanged,    // 列表内容变化（增/删）；带完整列表 + currentIndex
    PlaylistSwitched,   // 当前条目变化；各端据此重新解析该条目的来源
    PlaylistStatus,     // 各条目的匹配状态（服务端聚合后下发）

    // WebRTC 信令
    WebrtcOffer,
    WebrtcAnswer,
    WebrtcIce,
};

// 解码后的一条服务端消息。
//
// 刻意采用扁平结构 + type 判别，而非 std::variant<每种消息一个 struct>：
// 代价是字段未按 type 隔离（读错字段会得到默认值），换来调用方一次 switch
// 即可读完，无需模板分发。消息种类在 20 以内时该取舍是划算的。
struct Message {
    MessageType type = MessageType::Unknown;

    // welcome / room_created / room_joined
    // welcome 里是连接 id；room_created/room_joined 里是**稳定成员 id**——
    // 断线重连（resume）后仍与重连前相同。
    QString clientId;

    // room_created / room_joined
    QString roomId;
    QString roomName;
    QString mode;               // 线路格式："local" / "share" / "url"
    // 共享模式当前共享源（room_created / room_joined 顶层与 share_state 共用）：
    // "media"（房主本地电影）/ "screen" / "window"
    QString shareSource = QStringLiteral("media");
    bool shareAudio = false;
    QList<Member> members;
    bool videoMismatched = false;
    qint64 shortestDuration = 0;
    // 断线重连凭据。只保存在内存里，不落盘、不写日志。
    QString resumeToken;
    // 这次 room_joined 是 resume 的结果（房间页已经存在，不应重新 push）
    bool resumed = false;

    // playlist_changed / playlist_switched / state.playlist
    QList<PlaylistEntry> playlist;
    int currentIndex = -1;
    QString itemId;
    // video_status 的递增序号。服务端多线程处理消息，同一连接的相邻状态
    // 可能乱序到达；序号小的一方直接丢弃。
    qint64 seq = 0;

    // room_created / room_joined 的顶层字段：服务端下发的 ICE 服务器列表
    // （STUN + 带临时凭据的 TURN）。core 层只搬运，不认识 WebRTC；
    // 将其转换为 PeerConnection 配置的是 WebrtcManager。
    QList<IceServerConfig> iceServers;

    // member_joined / member_left（clientId + 下面这几个字段）
    QString nickname;
    bool isHost = false;
    bool loaded = false;
    qint64 duration = 0;

    // chat
    QString from;
    QString text;

    // playback：action + position（rate 仅换挡动作有意义）
    // room_joined 中的 state.playing / state.position / state.rate 也复用这几个字段
    QString action;
    bool hasState = false;
    bool playing = false;
    qint64 position = 0;
    qreal rate = 1.0;

    // webrtc_*
    //
    // link 指明该信令属于哪条 PeerConnection。一个对端有两条连接：
    // "voice"（语音）与 "media"（视频 + 电影）。两者是各自独立的 SDP/ICE 协商，
    // 必须分开路由，否则会串。服务端原样转发该字段，不解释其内容。
    QString link;
    QString sdp;
    QString sdpMid;
    int sdpMLineIndex = 0;

    // error
    QString code;
    QString errorMessage;
};

// 解析失败（非法 JSON / 缺少 type）返回 nullopt
std::optional<Message> decode(const QString &text);

// ---- 客户端 → 服务端：组包并序列化成可直接发送的文本 ----

QString encodeCreateRoom(const QString &roomId,
                         const QString &roomName,
                         const QString &nickname,
                         const QString &password);

QString encodeJoinRoom(const QString &roomId,
                       const QString &nickname,
                       const QString &password);

// 断线重连：用 roomId + 稳定 clientId + resumeToken 恢复原成员身份。
// 服务端成功时回一条 resumed=true 的 room_joined（clientId 不变）。
QString encodeResume(const QString &roomId,
                     const QString &clientId,
                     const QString &resumeToken);

QString encodeLeaveRoom();

QString encodeSetRoomMode(const QString &mode);

// 共享源（仅房主、仅共享模式）。source: "media" / "screen" / "window"
QString encodeSetShareState(const QString &source, bool audio);

QString encodeChat(const QString &text);

// action 为 "rate" 时携带 rate（1.0 = 原速）；其它动作忽略该参数
QString encodePlayback(const QString &action, qint64 position, qreal rate = 1.0);

QString encodePlaybackPosition(qint64 position, bool playing);

// itemId：这条状态属于哪个播放列表条目。服务端只比较当前条目的状态，
// 否则切换模式/切条目时，旧条目残留的 loaded/hash/duration 会被误判为不一致。
QString encodeVideoStatus(bool loaded, const QString &hash, qint64 duration,
                          const QString &itemId, qint64 seq);

// ---- 播放列表 ----
// itemId 由客户端生成（UUID），服务端只保证不重复。
// 本地文件路径不进协议，属各端自己的映射（见 docs/playlist-design.md）。
// spec 为画质选择字符串，语义见 FormatModel.h；服务端只存转发、不解释。
QString encodePlaylistAdd(const QString &itemId,
                          const QString &title,
                          const QString &url,
                          qint64 duration,
                          bool live,
                          const QString &spec);

QString encodePlaylistRemove(const QString &itemId);

QString encodePlaylistSwitch(const QString &itemId);

// 同步模式下成员上报「这一条本机是否有文件」。
// hash 由 MediaHash::forFile 计算，与 video_status 使用同一套算法
QString encodePlaylistStatus(const QString &itemId, bool hasFile, const QString &hash);

// link："voice" / "media"，见 Message::link 的说明
QString encodeWebrtcOffer(const QString &to, const QString &link, const QString &sdp);

QString encodeWebrtcAnswer(const QString &to, const QString &link, const QString &sdp);

QString encodeWebrtcIce(const QString &to,
                        const QString &link,
                        const QString &sdp,
                        const QString &sdpMid,
                        int sdpMLineIndex);

} // namespace Protocol

#endif //SYNCINE_PROTOCOL_H
