//
// 房间相关的传输层数据类型。
//


#ifndef SYNCINE_ROOMTYPES_H
#define SYNCINE_ROOMTYPES_H

#include <QList>
#include <QString>
#include <QStringList>

// 一条 ICE 服务器配置(服务端随 room_created / room_joined 下发)。
//
// 里面是服务端拿 Cloudflare 的 TURN 密钥换来的**临时**凭据 —— API token 是
// 账户级的,只待在服务端,客户端永远拿不到它(见 Server/src/tools/TurnCredentials.h)。
//
// 一组 urls 共用同一份 username/credential:Cloudflare 返回的 TURN 项就是这样,
// 一个 username 覆盖 udp/tcp/tls 好几个端口。STUN 那项没有凭据(也不需要)。
//
// 字段名照抄协议(credential);libwebrtc 那边叫 password,是同一个东西 ——
// 翻译只在 WebrtcManager::buildIceServers() 里做一次。
struct IceServerConfig {
    QStringList urls;
    QString username;
    QString credential;

    // 用来判断"配置有没有真的变"(每次建连前都会塞一遍同样的内容,
    // 内容没变就不必刷日志)
    bool operator==(const IceServerConfig &other) const = default;
};

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

    // 直播:没有时长、不能"下完再播" —— 各端各自用播放器直接拉流。
    // 直播下不比对内容哈希,也不做进度纠偏(没有共同时间轴)。
    bool live = false;

    // 画质选择,**spec 字符串**(空 = 最高画质)。语义见 FormatModel.h 顶部:
    //   "1080"            分辨率档(不超过、尽量接近 1080)
    //   "30102+30280"     详情视图手选的精确流
    //   末尾 ":h"         允许 HDR(设置里的开关,选档时缀上)
    //
    // 为什么是条目级、要跟着协议走:各人**各自下载**自己那份,选的不一样
    // 内容就不同、会被服务端判成"不同步"。所以它属于"大家共同认可的那份
    // 条目定义",不是个人偏好。服务端只存转发、不解释它。
    QString spec;

    // 由 spec 翻成的短标签("1080P" / "手选 1080P" / "最高画质"),只给界面显示用。
    // **不进协议** —— 各端用同一套 FormatModel 规则自己算,不占字段。
    QString specLabel;

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

    // 入房时服务端一并下发的 ICE 配置(STUN + 带临时凭据的 TURN)。
    // 本结构只负责"带着它",解释它的是 WebrtcManager。
    QList<IceServerConfig> iceServers;
};

#endif //SYNCINE_ROOMTYPES_H
