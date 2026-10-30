//
// 房间相关的传输层数据类型。
//


#ifndef SYNCINE_ROOMTYPES_H
#define SYNCINE_ROOMTYPES_H

#include <QList>
#include <QString>
#include <QStringList>

// 一条 ICE 服务器配置（服务端随 room_created / room_joined 下发）。
//
// 其中是服务端用 Cloudflare 的 TURN 密钥换取的临时凭据——API token 属账户级，
// 只留在服务端，客户端拿不到（见 Server/src/tools/TurnCredentials.h）。
//
// 一组 urls 共用同一份 username/credential：Cloudflare 返回的 TURN 项即如此，
// 一个 username 覆盖 udp/tcp/tls 多个端口。STUN 项没有凭据（也不需要）。
//
// 字段名照抄协议（credential）；libwebrtc 中称为 password，是同一项，
// 转换只在 WebrtcManager::buildIceServers() 中做一次。
struct IceServerConfig {
    QStringList urls;
    QString username;
    QString credential;

    // 用于判断配置是否真的变化（每次建连前都会写入同样的内容，
    // 内容未变则不必重复记录日志）
    bool operator==(const IceServerConfig &other) const = default;
};

// 房间成员的一次快照（服务端 members[] / member_joined 中的元素）
struct Member {
    QString clientId;
    QString nickname;
    bool isHost = false;
    // 断线等待重连的成员仍留在列表里，online=false。
    // 2A 阶段其他端暂时只按普通成员显示，后续可加"重连中"状态。
    bool online = true;
    bool loaded = false;
    qint64 duration = 0;
};

// 播放列表中的一条（服务端 playlist[] 中的元素）。
//
// 不含本地文件路径——路径属各端自己的映射，由客户端 PlaylistModel 另维护一份
// itemId → 本地文件 的对应关系。此处只有各端共同认可的条目定义。
struct PlaylistEntry {
    QString itemId;
    QString title;
    QString url;        // 可选，网链模式的源
    qint64 duration = 0; // 0 = 尚无人加载过，时长未知
    QString addedBy;

    // 直播：无时长、不能「下载完再播」，各端用播放器直接拉流。
    // 直播下不比对内容哈希，也不做进度纠偏（不存在共同时间轴）。
    bool live = false;

    // 画质选择，spec 字符串（空 = 最高画质）。语义见 FormatModel.h 顶部：
    //   "1080"          分辨率档（不超过、尽量接近 1080）
    //   "30102+30280"   详情视图手选的精确流
    //   末尾 ":h"       允许 HDR（设置中的开关，选档时缀上）
    //
    // 条目级而非个人偏好的原因：各端各自下载自己那份，选择不一致则内容不同、
    // 会被服务端判为不同步。故它属于各端共同认可的条目定义。
    // 服务端只存转发，不解释内容。
    QString spec;

    // 由 spec 转换出的短标签（"1080P" / "手选 1080P" / "最高画质"），仅供界面显示。
    // 不进协议——各端用同一套 FormatModel 规则自行计算，不占字段。
    QString specLabel;

    // 该条目的匹配状态，由服务端聚合（只有它掌握所有成员的情况）：
    //   "missing"  尚有人没有这一条 → 未匹配
    //   "matched"  全员都有且内容一致 → 已匹配
    //   "mismatch" 全员都有但内容不一致 → 不同步
    // 共享/网链模式不含此字段（这两种模式不存在匹配）
    QString status;
};

// 入场时服务端下发的完整房间状态（room_created / room_joined）
struct RoomSnapshot {
    QString roomId;
    QString roomName;
    QString mode;               // 线路格式："local" / "share" / "url"
    // 共享模式当前共享源："media" / "screen" / "window"
    QString shareSource = QStringLiteral("media");
    bool shareAudio = false;
    QString clientId;           // 自身的 clientId（来自 welcome 握手）
    QList<Member> members;
    bool hasState = false;      // 服务端是否下发了 state（未下发则不要对齐播放器）
    bool playing = false;       // state.playing
    qint64 position = 0;        // state.position
    qreal rate = 1.0;           // state.rate，缺省按原速
    bool videoMismatched = false;
    qint64 shortestDuration = 0;

    // 入房时服务端一并下发的播放列表（队列 + 当前条目）
    QList<PlaylistEntry> playlist;
    int currentIndex = -1;

    // 入房时服务端一并下发的 ICE 配置（STUN + 带临时凭据的 TURN）。
    // 本结构只负责携带，解释它的是 WebrtcManager。
    QList<IceServerConfig> iceServers;
};

#endif //SYNCINE_ROOMTYPES_H
