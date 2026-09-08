//
// Created by donggu on 2026/9/8.
//

#ifndef SERVER_CONFIG_H
#define SERVER_CONFIG_H

#include <string>

class Config {
    // 服务器配置
    // 后续完善从配置文件读取
public:
    inline static const std::string SERVER_ADDRESS = "0.0.0.0";
    inline static const unsigned short SERVER_PORT = 8765;
};

#endif //SERVER_CONFIG_H
