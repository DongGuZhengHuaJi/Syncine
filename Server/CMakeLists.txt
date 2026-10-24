cmake_minimum_required(VERSION 3.28)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(Server VERSION 1.2.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED)
find_package(Threads REQUIRED)

# TURN 凭据需向 Cloudflare 发起 HTTPS 请求：Boost.Beast 的 TLS 依赖 OpenSSL
# （Beast 自身为 header-only，没有可链接的库）。
find_package(OpenSSL REQUIRED)

# 协议消息用 nlohmann/json。Linux 上它在 /usr/include，不显式声明也能编；
# Windows（vcpkg）没有默认头文件搜索路径，必须把它的 include 目录带进来。
find_package(nlohmann_json CONFIG REQUIRED)

add_library(server_core STATIC
        src/server/Server.cpp
        src/server/Server.h
        src/network/WebSocketServer.cpp
        src/network/WebSocketServer.h
        src/network/Session.cpp
        src/network/Session.h
        src/network/SerialTaskQueue.cpp
        src/network/SerialTaskQueue.h
        src/network/OutboundQueue.cpp
        src/network/OutboundQueue.h
        src/network/CloseReason.h
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
        src/tools/ServerOptions.h
)

# 用 target_include_directories 而不是 include_directories：后者是目录级属性，
# 会污染同目录下所有目标。
target_include_directories(server_core
        PUBLIC
        src
        src/server
        src/network
        src/tools)

if(WIN32)
    # Boost.Asio 的 socket_types.hpp 会包含 <windows.h>。Windows 头默认定义
    # min/max 宏，会把 std::min/std::max 展开成语法错误（MSVC C2589/C2059）。
    target_compile_definitions(server_core PUBLIC NOMINMAX)
    target_compile_options(server_core PUBLIC /utf-8)
endif()

target_link_libraries(server_core
        PUBLIC
        Boost::boost
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
)

add_executable(Server src/main.cpp)
target_link_libraries(Server PRIVATE server_core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# 部署时 `cmake --install` 会把可执行文件放到 <prefix>/bin/Server。
include(GNUInstallDirs)
install(TARGETS Server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
