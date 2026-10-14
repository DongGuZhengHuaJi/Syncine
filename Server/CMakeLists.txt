cmake_minimum_required(VERSION 4.3)
project(Server)

set(CMAKE_CXX_STANDARD 23)


find_package(Boost REQUIRED)

# TURN 凭据要向 Cloudflare 发 HTTPS 请求:Boost.Beast 的 TLS 走的还是 OpenSSL
# (Beast 自己是 header-only,没有可链接的库)。
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
        # Secrets.h **不要列在这里**:它不进仓库(见 .gitignore),
        # 列了的话别人 clone 下来连 CMake 都配不过。
        # TurnCredentials.cpp 用 __has_include 找它,找不到就走环境变量。
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
