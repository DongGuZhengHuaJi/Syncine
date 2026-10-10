cmake_minimum_required(VERSION 4.3)
project(Server)

set(CMAKE_CXX_STANDARD 23)


find_package(Boost REQUIRED)

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
)
