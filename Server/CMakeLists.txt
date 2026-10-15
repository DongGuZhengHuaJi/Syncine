cmake_minimum_required(VERSION 4.3)
project(Server)

set(CMAKE_CXX_STANDARD 23)


find_package(Boost REQUIRED)

# TURN 凭据需向 Cloudflare 发起 HTTPS 请求：Boost.Beast 的 TLS 依赖 OpenSSL
# （Beast 自身为 header-only，没有可链接的库）。
find_package(OpenSSL REQUIRED)

add_executable(Server src/main.cpp
        src/server/Server.cpp
        src/server/Server.h
        src/network/WebSocketServer.cpp
        src/network/WebSocketServer.h
        src/network/Session.cpp
        src/network/Session.h
        src/network/Room.cpp
        src/network/Room.h
        src/tools/Log.cpp
        src/tools/Log.h
        src/tools/RandomGen.h
        src/tools/TurnCredentials.cpp
        src/tools/TurnCredentials.h
        # Secrets.h 不要列在此处：它不入仓库（见 .gitignore），
        # 列出会导致他人 clone 后连 CMake 配置都无法通过。
        # TurnCredentials.cpp 通过 __has_include 查找它，缺失时改走环境变量。
        src/network/LogicSystem.cpp
        src/network/LogicSystem.h
        src/tools/Config.h
)

include_directories(src
        src/server
        src/network
        src/tools)

target_link_libraries(Server
        PRIVATE
        Boost::boost
        OpenSSL::SSL
        OpenSSL::Crypto
)
