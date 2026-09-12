cmake_minimum_required(VERSION 3.28)

# ----------------------------------------------------------------------
# 1. 编译器
# ----------------------------------------------------------------------

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)

project(Syncine LANGUAGES CXX)

# WebRTC 使用 C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# gcc 14 对 WebRTC 头文件会误报 -Wchanges-meaning（clang 无此问题）
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-changes-meaning)
endif()

# 使用 lld
add_link_options(-fuse-ld=lld)

# ----------------------------------------------------------------------
# 2. Qt 自动化工具
# ----------------------------------------------------------------------

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ----------------------------------------------------------------------
# 3. 查找依赖
# ----------------------------------------------------------------------

find_package(Threads REQUIRED)

find_package(Qt6 REQUIRED COMPONENTS
        Core
        Gui
        Qml
        Quick
        QuickControls2
        Multimedia
        WebSockets
)

# ----------------------------------------------------------------------
# 4. WebRTC 路径
#
# WebRTC 源码：
# /home/donggu/webrtc-checkout/src/
#
# WebRTC 构建目录：
# /home/donggu/webrtc-checkout/src/out/Default/
#
# WebRTC 静态库：
# /home/donggu/webrtc-checkout/src/out/Default/obj/libwebrtc.a
# ----------------------------------------------------------------------

set(WEBRTC_ROOT
        "/home/donggu/webrtc-checkout/src"
)

set(WEBRTC_OUT
        "${WEBRTC_ROOT}/out/Default"
)

set(WEBRTC_LIBRARY
        "${WEBRTC_OUT}/obj/libwebrtc.a"
)

message(STATUS "WebRTC source : ${WEBRTC_ROOT}")
message(STATUS "WebRTC output : ${WEBRTC_OUT}")
message(STATUS "WebRTC library: ${WEBRTC_LIBRARY}")

# ----------------------------------------------------------------------
# 5. 检查 WebRTC 静态库是否存在
# ----------------------------------------------------------------------

if(NOT EXISTS "${WEBRTC_LIBRARY}")
    message(FATAL_ERROR
            "WebRTC library not found:\n"
            "  ${WEBRTC_LIBRARY}\n"
    )
endif()

# ----------------------------------------------------------------------
# 6. 创建 WebRTC IMPORTED Target
#
# 这里不是重新编译 WebRTC。
# CMake 只是告诉它：
#
#   头文件在哪里
#   已经编译好的 libwebrtc.a 在哪里
# ----------------------------------------------------------------------

add_library(WebRTC::WebRTC STATIC IMPORTED GLOBAL)

set_target_properties(WebRTC::WebRTC PROPERTIES

        # WebRTC 静态库
        IMPORTED_LOCATION
        "${WEBRTC_LIBRARY}"

        # WebRTC 头文件根目录
        #
        # 这样：
        #
        # #include "api/peer_connection_interface.h"
        #
        # 就会从：
        #
        # /home/donggu/webrtc-checkout/src/api/...
        #
        # 查找
        INTERFACE_INCLUDE_DIRECTORIES
        "${WEBRTC_ROOT};${WEBRTC_OUT}/gen;${WEBRTC_ROOT}/third_party/abseil-cpp;${WEBRTC_ROOT}/third_party/perfetto/include;${WEBRTC_OUT}/gen/third_party/perfetto;${WEBRTC_OUT}/gen/third_party/perfetto/build_config;${WEBRTC_ROOT}/third_party/libyuv/include"
)

# ----------------------------------------------------------------------
# 7. WebRTC 编译宏
# ----------------------------------------------------------------------

target_compile_definitions(WebRTC::WebRTC INTERFACE
        WEBRTC_POSIX
        WEBRTC_LINUX
        _FILE_OFFSET_BITS=64
        __STDC_CONSTANT_MACROS
        __STDC_FORMAT_MACROS
        ABSL_ALLOCATOR_NOTHROW=1
        RTC_ENABLE_VP9
        WEBRTC_HAVE_SCTP
        WEBRTC_ENABLE_PROTOBUF=1
        WEBRTC_STRICT_FIELD_TRIALS=0
        _GLIBCXX_ASSERTIONS=1
)

# ----------------------------------------------------------------------
# 8. WebRTC 当前已知的系统依赖
#
# 注意：
# WebRTC 是一个非常大的静态库。
# 这里先放当前明确需要的系统库。
#
# 如果后面链接阶段出现：
#
#   undefined reference to ...
#
# 再根据具体报错补 WebRTC 的其他依赖。
# ----------------------------------------------------------------------

target_link_libraries(WebRTC::WebRTC INTERFACE
        Threads::Threads
        ${CMAKE_DL_LIBS}
        atomic
        X11
        Xcomposite
        Xdamage
        Xext
        Xfixes
        Xrandr
        Xrender
        Xtst
)

# ----------------------------------------------------------------------
# 9. Qt 项目初始化
# ----------------------------------------------------------------------

qt_standard_project_setup()

# ----------------------------------------------------------------------
# 10. Syncine 可执行文件
# ----------------------------------------------------------------------

qt_add_executable(Syncine

        src/main.cpp

        # Playback
        src/PlaybackController.cpp
        src/PlaybackController.h

        # Room
        src/RoomManager.cpp
        src/RoomManager.h

        # Network
        src/NetworkManager.cpp
        src/NetworkManager.h

        # WebRTC
        src/WebrtcManager.cpp
        src/WebrtcManager.h
)

# ----------------------------------------------------------------------
# 11. Syncine 自己的头文件目录
# ----------------------------------------------------------------------

target_include_directories(Syncine
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# ----------------------------------------------------------------------
# 12. QML 单例
# ----------------------------------------------------------------------

set_source_files_properties(
        ui/Style.qml
        PROPERTIES
        QT_QML_SINGLETON_TYPE TRUE
)

# ----------------------------------------------------------------------
# 13. QML 模块
# ----------------------------------------------------------------------

qt_add_qml_module(Syncine
        URI SyncineApp
        VERSION 1.0
        RESOURCE_PREFIX /qt/qml

        QML_FILES

        # Root
        ui/Main.qml
        ui/Style.qml

        # Components
        ui/components/Card.qml
        ui/components/PrimaryButton.qml
        ui/components/GhostButton.qml
        ui/components/MessageDialog.qml
        ui/components/AppTextField.qml
        ui/components/ModeButton.qml
        ui/components/VideoPlayer.qml
        ui/components/PlaybackControl.qml
        ui/components/PlaybackSeekControl.qml

        # Pages
        ui/pages/HomePage.qml
        ui/pages/CreateRoomPage.qml
        ui/pages/JoinRoomPage.qml
        ui/pages/RoomPage.qml
)

# ----------------------------------------------------------------------
# 14. 链接所有依赖
# ----------------------------------------------------------------------

target_link_libraries(Syncine
        PRIVATE

        # Qt
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Multimedia
        Qt6::WebSockets

        # WebRTC
        WebRTC::WebRTC
)