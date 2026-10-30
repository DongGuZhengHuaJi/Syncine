//
// Created by donggu on 2026/9/13.
//

#include "Protocol.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/Log.h"

namespace {

using Protocol::Message;
using Protocol::MessageType;

MessageType typeFromString(const QString &type) {
    static const QHash<QString, MessageType> table{
        {"welcome", MessageType::Welcome},
        {"room_created", MessageType::RoomCreated},
        {"room_joined", MessageType::RoomJoined},
        {"room_left", MessageType::RoomLeft},
        {"member_joined", MessageType::MemberJoined},
        {"member_left", MessageType::MemberLeft},
        {"member_online", MessageType::MemberOnline},
        {"member_offline", MessageType::MemberOffline},
        {"chat", MessageType::Chat},
        {"playback", MessageType::Playback},
        {"playback_position", MessageType::PlaybackPosition},
        {"room_mode_changed", MessageType::RoomModeChanged},
        {"share_state", MessageType::ShareState},
        {"video_status", MessageType::VideoStatus},
        {"video_mismatch", MessageType::VideoMismatch},
        {"playlist_changed", MessageType::PlaylistChanged},
        {"playlist_switched", MessageType::PlaylistSwitched},
        {"playlist_status", MessageType::PlaylistStatus},
        {"webrtc_offer", MessageType::WebrtcOffer},
        {"webrtc_answer", MessageType::WebrtcAnswer},
        {"webrtc_ice", MessageType::WebrtcIce},
        {"error", MessageType::Error},
    };
    return table.value(type, MessageType::Unknown);
}

// JSON 里的数字都是 double,qint64 字段统一走这里，避免各处手写 static_cast
qint64 int64Of(const QJsonObject &object, const QString &key) {
    return static_cast<qint64>(object.value(key).toDouble());
}

Member memberFromJson(const QJsonObject &object) {
    Member member;
    member.clientId = object.value("clientId").toString();
    member.nickname = object.value("nickname").toString();
    member.isHost = object.value("isHost").toBool(false);
    member.online = object.value("online").toBool();
    member.loaded = object.value("loaded").toBool(false);
    member.duration = int64Of(object, "duration");
    return member;
}

PlaylistEntry playlistEntryFromJson(const QJsonObject &object) {
    PlaylistEntry entry;
    entry.itemId = object.value("itemId").toString();
    entry.title = object.value("title").toString();
    entry.url = object.value("url").toString();
    entry.duration = int64Of(object, "duration");
    entry.addedBy = object.value("addedBy").toString();
    entry.status = object.value("status").toString();
    entry.live = object.value("live").toBool(false);
    entry.spec = object.value("spec").toString();
    return entry;
}

QList<PlaylistEntry> playlistFromJson(const QJsonValue &value) {
    QList<PlaylistEntry> entries;
    const QJsonArray array = value.toArray();
    entries.reserve(array.size());
    for (const QJsonValue &item : array)
        entries.append(playlistEntryFromJson(item.toObject()));
    return entries;
}

// urls 按 WebRTC 的定义允许写成字符串或字符串数组。
// Cloudflare 现在返回的是数组，但两种都收着点 —— 形状一变不至于整批废掉。
QStringList stringListFromJson(const QJsonValue &value) {
    QStringList list;
    if (value.isString()) {
        list.append(value.toString());
        return list;
    }

    const QJsonArray array = value.toArray();
    list.reserve(array.size());
    for (const QJsonValue &item : array)
        list.append(item.toString());
    return list;
}

IceServerConfig iceServerFromJson(const QJsonObject &object) {
    IceServerConfig server;
    server.urls = stringListFromJson(object.value("urls"));
    // STUN 那一项没有这两个字段，取到空串是正常的（它不需要鉴权）
    server.username = object.value("username").toString();
    server.credential = object.value("credential").toString();
    return server;
}

QString serialize(const QJsonObject &object) {
    return QString::fromUtf8(
        QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace

namespace Protocol {

std::optional<Message> decode(const QString &text) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        LOG_WARN("Protocol") << "消息解析失败:" << parseError.errorString()
                             << " 原文(前 200 字符):" << text.left(200);
        return std::nullopt;
    }

    const QJsonObject object = doc.object();

    // 缺少 type 字段说明这不是一条协议消息，与 JSON 损坏同样按解析失败处理；
    // 而 type 存在但服务端新增了类型，则归为 Unknown——调用方静默跳过即可，
    // 不应因为出现未知类型就向用户报错
    const QString type = object.value("type").toString();
    if (type.isEmpty()) {
        LOG_WARN("Protocol") << "消息缺少 type 字段，丢弃。原文(前 200 字符):"
                             << text.left(200);
        return std::nullopt;
    }

    Message message;
    message.type = typeFromString(type);

    switch (message.type) {
    case MessageType::Welcome:
        message.clientId = object.value("clientId").toString();
        break;

    case MessageType::RoomCreated:
    case MessageType::RoomJoined: {
        message.roomId = object.value("roomId").toString();
        message.clientId = object.value("clientId").toString();
        message.resumeToken = object.value("resumeToken").toString();
        message.resumed = object.value("resumed").toBool(false);
        message.roomName = object.value("roomName").toString();
        message.mode = object.value("mode").toString();
        // 旧服务端没有这两个字段：默认按「房主共享本地电影」处理，
        // 与旧版共享模式的语义一致。
        message.shareSource = object.value("shareSource").toString();
        if (message.shareSource.isEmpty())
            message.shareSource = QStringLiteral("media");
        message.shareAudio = object.value("shareAudio").toBool(false);
        message.videoMismatched = object.value("videoMismatched").toBool(false);
        message.shortestDuration = int64Of(object, "shortestDuration");

        const QJsonArray members = object.value("members").toArray();
        message.members.reserve(members.size());
        for (const QJsonValue &value : members)
            message.members.append(memberFromJson(value.toObject()));

        const QJsonObject state = object.value("state").toObject();
        message.hasState = !state.isEmpty();
        message.playing = state.value("playing").toBool();
        message.position = int64Of(state, "position");
        message.rate = state.value("rate").toDouble(1.0);
        message.playlist = playlistFromJson(state.value("playlist"));
        message.currentIndex = state.contains("currentIndex")
                                   ? state.value("currentIndex").toInt(-1)
                                   : -1;

        // ICE 配置在**顶层**(不在 state 里):它不是房间播放状态，是网络配置
        const QJsonArray iceServers = object.value("iceServers").toArray();
        message.iceServers.reserve(iceServers.size());
        for (const QJsonValue &value : iceServers)
            message.iceServers.append(iceServerFromJson(value.toObject()));
        break;
    }

    case MessageType::MemberJoined:
    case MessageType::MemberLeft:
    case MessageType::MemberOnline:
    case MessageType::MemberOffline:
        message.clientId = object.value("clientId").toString();
        message.nickname = object.value("nickname").toString();
        message.isHost = object.value("isHost").toBool(false);
        message.loaded = object.value("loaded").toBool(false);
        message.duration = int64Of(object, "duration");
        break;

    case MessageType::Chat:
        message.from = object.value("from").toString();
        message.text = object.value("text").toString();
        break;

    case MessageType::Playback:
        message.action = object.value("action").toString();
        // rate 动作可能不带 position（发送方还没加载媒体，位置未知）
        message.position = object.contains("position")
                               ? int64Of(object, "position")
                               : -1;
        message.rate = object.value("rate").toDouble(1.0);
        break;

    case MessageType::PlaybackPosition:
        message.position = int64Of(object, "position");
        message.playing = object.value("playing").toBool(false);
        break;

    case MessageType::RoomModeChanged:
        message.mode = object.value("mode").toString();
        break;

    case MessageType::ShareState:
        message.shareSource = object.value("source").toString();
        if (message.shareSource.isEmpty())
            message.shareSource = QStringLiteral("media");
        message.shareAudio = object.value("audio").toBool(false);
        break;

    case MessageType::VideoStatus:
        message.clientId = object.value("clientId").toString();
        message.loaded = object.value("loaded").toBool(false);
        message.duration = int64Of(object, "duration");
        message.itemId = object.value("itemId").toString();
        message.seq = int64Of(object, "seq");
        break;

    case MessageType::VideoMismatch:
        message.videoMismatched = object.value("mismatched").toBool(false);
        message.shortestDuration = int64Of(object, "shortestDuration");
        break;

    case MessageType::PlaylistChanged:
        message.playlist = playlistFromJson(object.value("items"));
        message.currentIndex = object.value("currentIndex").toInt(-1);
        break;

    case MessageType::PlaylistSwitched:
        message.itemId = object.value("itemId").toString();
        message.currentIndex = object.value("currentIndex").toInt(-1);
        break;

    case MessageType::PlaylistStatus:
        // 服务端聚合后下发的形式：items[] 里每条只有 itemId + status。
        // 复用 PlaylistEntry 承载（其余字段为空）,客户端只取这两个
        message.playlist = playlistFromJson(object.value("items"));
        break;

    case MessageType::WebrtcOffer:
    case MessageType::WebrtcAnswer:
    case MessageType::WebrtcIce:
        message.from = object.value("from").toString();
        message.link = object.value("link").toString();
        message.sdp = object.value("sdp").toString();
        message.sdpMid = object.value("sdpMid").toString();
        message.sdpMLineIndex = object.value("sdpMLineIndex").toInt();
        break;

    case MessageType::Error:
        message.code = object.value("code").toString();
        message.errorMessage = object.value("message").toString();
        break;

    case MessageType::RoomLeft:
    case MessageType::Unknown:
        break;
    }

    return message;
}

QString encodeCreateRoom(const QString &roomId,
                         const QString &roomName,
                         const QString &nickname,
                         const QString &password) {
    QJsonObject message;
    message["type"] = "create_room";
    message["roomId"] = roomId;
    message["roomName"] = roomName;
    message["nickname"] = nickname;
    if (!password.isEmpty())
        message["password"] = password;
    return serialize(message);
}

QString encodeJoinRoom(const QString &roomId,
                       const QString &nickname,
                       const QString &password) {
    QJsonObject message;
    message["type"] = "join_room";
    message["roomId"] = roomId;
    message["nickname"] = nickname;
    if (!password.isEmpty())
        message["password"] = password;
    return serialize(message);
}

QString encodeResume(const QString &roomId,
                     const QString &clientId,
                     const QString &resumeToken) {
    QJsonObject message;
    message["type"] = "resume";
    message["roomId"] = roomId;
    message["clientId"] = clientId;
    message["token"] = resumeToken;
    return serialize(message);
}

QString encodeLeaveRoom() {
    QJsonObject message;
    message["type"] = "leave_room";
    return serialize(message);
}

QString encodeSetRoomMode(const QString &mode) {
    QJsonObject message;
    message["type"] = "set_room_mode";
    message["mode"] = mode;
    return serialize(message);
}

QString encodeSetShareState(const QString &source, bool audio) {
    QJsonObject message;
    message["type"] = "set_share_state";
    message["source"] = source;
    message["audio"] = audio;
    return serialize(message);
}

QString encodeChat(const QString &text) {
    QJsonObject message;
    message["type"] = "chat";
    message["text"] = text;
    return serialize(message);
}

QString encodePlayback(const QString &action, qint64 position, qreal rate) {
    QJsonObject message;
    message["type"] = "playback";
    message["action"] = action;
    // 换挡时 position 为负表示"发送方没有已加载的媒体，位置未知"，
    // 此时不带该字段，接收方只换挡、不对齐位置，避免把别人拖到片头。
    if (position >= 0)
        message["position"] = position;
    // 只有换挡动作需要 rate；其它动作不带，按 1.0 处理
    if (action == QLatin1String("rate"))
        message["rate"] = rate;
    return serialize(message);
}

QString encodePlaybackPosition(qint64 position, bool playing) {
    QJsonObject message;
    message["type"] = "playback_position";
    message["position"] = position;
    message["playing"] = playing;
    return serialize(message);
}

QString encodeVideoStatus(bool loaded, const QString &hash, qint64 duration,
                          const QString &itemId, qint64 seq) {
    QJsonObject message;
    message["type"] = "video_status";
    message["loaded"] = loaded;
    message["hash"] = loaded ? hash : QString();
    message["duration"] = loaded ? duration : 0;
    // loaded=false 也带上 itemId：它表示「卸载的是这一条」，
    // 服务端据此丢弃切模式后才迟到的旧条目状态。
    message["itemId"] = itemId;
    message["seq"] = seq;
    return serialize(message);
}

QString encodePlaylistAdd(const QString &itemId,
                          const QString &title,
                          const QString &url,
                          qint64 duration,
                          bool live,
                          const QString &spec) {
    QJsonObject message;
    message["type"] = "playlist_add";
    message["itemId"] = itemId;
    message["title"] = title;
    // url / duration / spec 是可选信息，空着就不发，省得服务端存一堆空字段
    if (!url.isEmpty())
        message["url"] = url;
    if (duration > 0)
        message["duration"] = duration;
    if (live)
        message["live"] = true;
    if (!spec.isEmpty())
        message["spec"] = spec;
    return serialize(message);
}

QString encodePlaylistRemove(const QString &itemId) {
    QJsonObject message;
    message["type"] = "playlist_remove";
    message["itemId"] = itemId;
    return serialize(message);
}

QString encodePlaylistSwitch(const QString &itemId) {
    QJsonObject message;
    message["type"] = "playlist_switch";
    message["itemId"] = itemId;
    return serialize(message);
}

QString encodePlaylistStatus(const QString &itemId, bool hasFile, const QString &hash) {
    QJsonObject message;
    message["type"] = "playlist_status";
    message["itemId"] = itemId;
    message["hasFile"] = hasFile;
    if (hasFile)
        message["hash"] = hash;
    return serialize(message);
}

QString encodeWebrtcOffer(const QString &to, const QString &link, const QString &sdp) {
    QJsonObject message;
    message["type"] = "webrtc_offer";
    message["to"] = to;
    message["link"] = link;
    message["sdp"] = sdp;
    return serialize(message);
}

QString encodeWebrtcAnswer(const QString &to, const QString &link, const QString &sdp) {
    QJsonObject message;
    message["type"] = "webrtc_answer";
    message["to"] = to;
    message["link"] = link;
    message["sdp"] = sdp;
    return serialize(message);
}

QString encodeWebrtcIce(const QString &to,
                        const QString &link,
                        const QString &sdp,
                        const QString &sdpMid,
                        int sdpMLineIndex) {
    QJsonObject message;
    message["type"] = "webrtc_ice";
    message["to"] = to;
    message["link"] = link;
    message["sdp"] = sdp;
    message["sdpMid"] = sdpMid;
    message["sdpMLineIndex"] = sdpMLineIndex;
    return serialize(message);
}

} // namespace Protocol
