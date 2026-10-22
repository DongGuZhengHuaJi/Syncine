//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include "ServerOptions.h"

#include <cstddef>
#include <string>

class Config {
public:
    // 生产默认值。测试通过显式注入 ServerOptions 缩短超时、缩小队列。
    inline static const std::string SERVER_ADDRESS = "0.0.0.0";
    inline static const unsigned short SERVER_PORT = 8765;

    // ---- 配额 ----
    //
    // 服务端对公网开放，而房间号只有 6 位、口令由用户自定，这些上限是
    // 「一个连接就能把服务端撑爆」的兜底：没有它们时，单连接循环
    // playlist_add 即可让列表无限增长，而每次添加都会全量序列化整个列表
    // 并群发（成本 O(n²)），且全程持有全局房间锁 —— 一个房间的灌数据会
    // 卡住所有房间。
    //
    // 取值按「一起看」的实际用量留了充足余量，正常使用碰不到。

    // 同时存在的房间数。房间号空间是 100 万，但真实并发远低于此
    inline static const std::size_t MAX_ROOMS = 500;
    // 单个房间的成员上限
    inline static const std::size_t MAX_MEMBERS_PER_ROOM = 16;
    // 单个模式下的播放列表条目上限（三种模式各存一份，故各自独立计数）
    inline static const std::size_t MAX_PLAYLIST_ENTRIES = 200;

    // 单条 WebSocket 消息的上限（字节）。
    // Beast 默认 16 MB，对本协议的任何合法消息都过大了：最大的字段是
    // title / url，几十 KB 足够，而日志里会记 title（见 LogicSystem 的
    // 「播放列表添加」），16 MB 的 title 能把日志轮转冲垮。
    inline static const std::size_t MAX_MESSAGE_BYTES = 64 * 1024;

    static ServerOptions serverOptions() {
        ServerOptions options;
        options.address = SERVER_ADDRESS;
        options.port = SERVER_PORT;
        options.maxMessageBytes = MAX_MESSAGE_BYTES;
        return options;
    }
};

#endif //SERVER_CONFIG_H
