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
#
# 这一节不是"随便加的开关"。下面每个宏都必须和编译 libwebrtc.a 时
# 用的那一套完全一致 —— WebRTC 头文件里有大量 `#if <宏>` 包裹的条件成员,
# 一旦不一致,同一个类在库里和客户端里 sizeof 就不同,内存布局错位。
#
# NDEBUG 就是这么踩出来的(症状:启动即 SIGSEGV):
#   rtc_base/thread.h 里 webrtc::Thread 有 4 个成员被 `#if RTC_DCHECK_IS_ON`
#   包着,而 RTC_DCHECK_IS_ON = !defined(NDEBUG) || defined(DCHECK_ALWAYS_ON)。
#   libwebrtc.a 是 is_debug = false 编的(带 -DNDEBUG,RTC_DCHECK_IS_ON = 0),
#   客户端不带 NDEBUG 就是 1 —— 实测 sizeof(webrtc::Thread) 256 vs 312,
#   own_ss_ 偏移差 56 字节。于是 CreateWithSocketServer() 里
#   `own_ss_ = std::move(ss)` 读到的是垃圾指针,销毁旧值时崩在虚析构调用上。
#
# 所以这里 NDEBUG 的含义是"和库的 ABI 对齐",不是"我要出发布版"。
# 它必须独立于 CMAKE_BUILD_TYPE:客户端哪怕切成 Debug 也照样得定义。
# ----------------------------------------------------------------------

target_compile_definitions(WebRTC::WebRTC INTERFACE
        NDEBUG # ABI 对齐用,勿删 —— 理由见上;WebrtcManager.cpp 有编译期护栏
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
# 9. 链接器:必须用认识 CREL 重定位的 lld(>= LLVM 19.1)
#
# libwebrtc.a 是用 Chromium 自带的 clang 24 编的,对象文件里用的是实验性的
# CREL 重定位格式 —— 节名是 `.crel.text` 而不是标准的 `.rela.text`。
#
# 系统自带的 lld 18 不认识 CREL —— 它不报错,只是把 `.crel.*` 当未知节静默
# 忽略。后果是库里所有重定位一个都没被施加:每个 call 的位移都是 0,调用会
# "穿透"到紧随其后的下一条指令,把未初始化的寄存器当成函数返回值。
# 症状是程序启动即在 webrtc::Thread::CreateWithSocketServer() 里段错误,
# 而且 gdb 看到的是"往一个垃圾指针上做虚调用",从代码层面完全看不出问题。
#
# 随 WebRTC 一起构建出来的 lld 就认识 CREL,直接指过去即可(它比 -fuse-ld
# 优先级高,所以系统里原有的 -fuse-ld=lld 会被覆盖)。
# ----------------------------------------------------------------------

set(WEBRTC_LLD
        "${WEBRTC_ROOT}/third_party/llvm-build/Release+Asserts/bin/ld.lld"
)

if(NOT EXISTS "${WEBRTC_LLD}")
    message(FATAL_ERROR
            "找不到支持 CREL 重定位的链接器:\n"
            "  ${WEBRTC_LLD}\n"
            "libwebrtc.a 的对象文件使用 CREL 重定位,用系统 lld(< 19.1) 链接不会报错,\n"
            "但会产出一个所有跨函数调用都失效的可执行文件(启动即段错误)。\n"
    )
endif()

# 挂在 WebRTC 这个 IMPORTED target 上,链接它的目标自动继承
set_target_properties(WebRTC::WebRTC PROPERTIES
        INTERFACE_LINK_OPTIONS "--ld-path=${WEBRTC_LLD}"
)

# ----------------------------------------------------------------------
# 10. Qt 项目初始化
# ----------------------------------------------------------------------

qt_standard_project_setup()

# ----------------------------------------------------------------------
# 11. Syncine 可执行文件
# ----------------------------------------------------------------------

qt_add_executable(Syncine

        src/main.cpp

        # core:传输 + 协议(不认识房间,也不认识播放器)
        src/core/NetworkManager.cpp
        src/core/NetworkManager.h
        src/core/Protocol.cpp
        src/core/Protocol.h
        src/core/RoomTypes.h

        # playback:本地播放器 + 播放同步策略
        src/playback/PlaybackController.cpp
        src/playback/PlaybackController.h
        src/playback/PlaybackSync.cpp
        src/playback/PlaybackSync.h

        # session:房间与会话
        src/session/MemberModel.cpp
        src/session/MemberModel.h
        src/session/RoomSession.cpp
        src/session/RoomSession.h
        src/session/SessionController.cpp
        src/session/SessionController.h

        # webrtc
        src/webrtc/WebrtcManager.cpp
        src/webrtc/WebrtcManager.h
        src/webrtc/PeerLink.cpp
        src/webrtc/PeerLink.h
        src/webrtc/SignalingChannel.cpp
        src/webrtc/SignalingChannel.h
        src/webrtc/VideoTrackSource.cpp
        src/webrtc/VideoTrackSource.h
        src/webrtc/RemoteVideoRenderer.cpp
        src/webrtc/RemoteVideoRenderer.h
)

# ----------------------------------------------------------------------
# 12. Syncine 自己的头文件目录
#
# src/ 是为了让源码里能写 "session/RoomSession.h" 这种带分层的路径;
# 各子目录本身也要加进来,是因为 qmltyperegistrar 生成的
# syncine_qmltyperegistrations.cpp 只会用基名 include(<RoomSession.h>)。
# ----------------------------------------------------------------------

target_include_directories(Syncine
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/playback"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/session"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc"
)

# ----------------------------------------------------------------------
# 13. QML 单例
# ----------------------------------------------------------------------

set_source_files_properties(
        ui/Style.qml
        PROPERTIES
        QT_QML_SINGLETON_TYPE TRUE
)

# ----------------------------------------------------------------------
# 14. QML 模块
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
# 15. 链接所有依赖
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