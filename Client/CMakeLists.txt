cmake_minimum_required(VERSION 3.28)

# ----------------------------------------------------------------------
# 1. 编译器
#
# 必须在 project() 之前设定 —— project() 一旦执行，工具链就锁定了，
# 之后再改 CMAKE_C_COMPILER 不会生效。
#
# 必须 clang：libwebrtc.a 是 Chromium 的 clang 编的，且对象文件使用 CREL
# 重定位（见 cmake/FindWebRTC.cmake 第 7 节）。传 -DCMAKE_CXX_COMPILER=...
# 可以覆盖。
# ----------------------------------------------------------------------

if(NOT CMAKE_C_COMPILER OR NOT CMAKE_CXX_COMPILER)
    find_program(SYNCINE_CLANG NAMES clang)
    find_program(SYNCINE_CLANGXX NAMES clang++)
    if(NOT SYNCINE_CLANG OR NOT SYNCINE_CLANGXX)
        message(FATAL_ERROR "未找到 clang/clang++，本项目依赖 clang 工具链（见本文件注释）。")
    endif()
    set(CMAKE_C_COMPILER "${SYNCINE_CLANG}")
    set(CMAKE_CXX_COMPILER "${SYNCINE_CLANGXX}")
endif()

# ----------------------------------------------------------------------
# 2. 版本号
#
# 这里是全项目版本号的唯一来源：CPack 的包名、QML 模块版本、version.h 里的
# SYNCINE_VERSION 都由它派生。改版本只改这一行。
# ----------------------------------------------------------------------

project(Syncine VERSION 1.0.0 LANGUAGES CXX)

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

# RTTI 必须关掉，和 libwebrtc.a 保持一致。
#
# libwebrtc.a 是 -fno-rtti 编的（Chromium 默认），归档里**没有任何 typeinfo 符号**。
# 我们这边开着 RTTI 时，派生自 webrtc 接口的类会生成自己的 typeinfo，而它需要
# 引用基类的 typeinfo —— 于是链接期报：
#     undefined symbol: typeinfo for webrtc::AudioSourceInterface
#
# 为什么只有部分接口会炸：Itanium ABI 规定，若一个类有「key function」
# （第一个非内联的虚函数，定义在 .cc 里），它的 vtable/typeinfo 只在定义该函数的
# 那个 TU 里生成一次。AudioSourceInterface::options() 正是这种情况，所以它的
# typeinfo 只存在于 libwebrtc 的 .o 里 —— 而那里面因为 -fno-rtti 是空的。
# VideoTrackSourceInterface 没有 key function，每个 TU 各自生成弱符号，所以不炸。
#
# 关掉 RTTI 对本项目无副作用：src/ 下没有任何 dynamic_cast/typeid。
add_compile_options(-fno-rtti)

# ----------------------------------------------------------------------
# 3. Qt 自动化工具
# ----------------------------------------------------------------------

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ----------------------------------------------------------------------
# 4. 查找依赖
# ----------------------------------------------------------------------

find_package(Threads REQUIRED)

find_package(Qt6 REQUIRED COMPONENTS
        Core
        Gui
        Qml
        Quick
        QuickControls2
        Multimedia
        Network
        WebSockets
)

# ----------------------------------------------------------------------
# 5. WebRTC
#
# 路径解析、宏对齐、系统库、CREL 链接器都在 cmake/FindWebRTC.cmake 里，
# 那份注释同样重要，改动前先读它。
#
# 指定目录的方式（按优先级）：
#   -DWEBRTC_ROOT=/path/to/webrtc-checkout/src
#   $ENV{WEBRTC_ROOT}
# ----------------------------------------------------------------------

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
find_package(WebRTC REQUIRED)

# ----------------------------------------------------------------------
# 6. 版本头文件
#
# 把 CMake 里的版本号导出给 C++ 代码（「关于」对话框等）。
# ----------------------------------------------------------------------

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/version.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/version.h"
        @ONLY
)

# ----------------------------------------------------------------------
# 7. Qt 项目初始化
# ----------------------------------------------------------------------

qt_standard_project_setup()

# ----------------------------------------------------------------------
# 8. Syncine 可执行文件
# ----------------------------------------------------------------------

qt_add_executable(Syncine

        src/main.cpp

        # core：传输 + 协议（不认识房间，也不认识播放器）
        src/core/AppSettings.cpp
        src/core/AppSettings.h
        src/core/AudioDevices.h
        src/core/CookieManager.cpp
        src/core/CookieManager.h
        src/core/HdrConverter.cpp
        src/core/HdrConverter.h
        src/core/Log.cpp
        src/core/Log.h
        src/core/MediaHash.cpp
        src/core/MediaHash.h
        src/core/NetworkManager.cpp
        src/core/NetworkManager.h
        src/core/Toolchain.cpp
        src/core/Toolchain.h
        src/core/UrlDownloader.cpp
        src/core/UrlDownloader.h
        src/core/Protocol.cpp
        src/core/Protocol.h
        src/core/RoomTypes.h

        # media：格式模型与探测（不认识房间，也不认识播放器）
        src/media/FormatModel.cpp
        src/media/FormatModel.h
        src/media/UrlInspector.cpp
        src/media/UrlInspector.h

        # playback：本地播放器 + 播放同步策略
        src/playback/PlaybackController.cpp
        src/playback/PlaybackController.h
        src/playback/PlaybackSync.cpp
        src/playback/PlaybackSync.h

        # session：房间与会话
        src/session/MemberModel.cpp
        src/session/MemberModel.h
        src/session/PlaylistModel.cpp
        src/session/PlaylistModel.h
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
        src/webrtc/MovieAudioSource.cpp
        src/webrtc/MovieAudioSource.h
        src/webrtc/MovieAudioPlayer.cpp
        src/webrtc/MovieAudioPlayer.h
)

# ----------------------------------------------------------------------
# 9. Syncine 自己的头文件目录
#
# src/ 是为了让源码里能写 "session/RoomSession.h" 这种带分层的路径；
# 各子目录本身也要加进来，是因为 qmltyperegistrar 生成的
# syncine_qmltyperegistrations.cpp 只会用基名 include(<RoomSession.h>)。
# ----------------------------------------------------------------------

target_include_directories(Syncine
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/media"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/playback"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/session"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc"
        "${CMAKE_CURRENT_BINARY_DIR}" # version.h
)

# ----------------------------------------------------------------------
# 10. QML 单例
# ----------------------------------------------------------------------

set_source_files_properties(
        ui/Style.qml
        PROPERTIES
        QT_QML_SINGLETON_TYPE TRUE
)

# ----------------------------------------------------------------------
# 11. QML 模块
# ----------------------------------------------------------------------

qt_add_qml_module(Syncine
        URI SyncineApp
        VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
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
        ui/components/AddVideoDialog.qml
        ui/components/AudioDeviceSettings.qml
        ui/components/AudioSettingsDialog.qml
        ui/components/OptionList.qml
        ui/components/FormatList.qml
        ui/components/AppTextField.qml
        ui/components/ModeButton.qml
        ui/components/AppIcon.qml
        ui/components/ControlButton.qml
        ui/components/VolumeButton.qml
        ui/components/DanmakuBar.qml
        ui/components/DanmakuLayer.qml
        ui/components/FunctionPanel.qml
        ui/components/VideoPlayer.qml
        ui/components/PlaybackControl.qml
        ui/components/PlaybackSeekControl.qml

        # Pages
        ui/pages/SettingsPage.qml
        ui/pages/HomePage.qml
        ui/pages/CreateRoomPage.qml
        ui/pages/JoinRoomPage.qml
        ui/pages/RoomPage.qml
)

# ----------------------------------------------------------------------
# 12. 链接所有依赖
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
        Qt6::Network
        Qt6::WebSockets

        # WebRTC
        WebRTC::WebRTC
)

# ======================================================================
# 13. 安装规则 —— 打包的基石
#
# CPack 只会打包 install() 出去的东西。这里的布局必须和运行时的查找路径
# 对齐：装成 <prefix>/bin/Syncine + <prefix>/tools/<工具>/<可执行文件> 后，
# Toolchain::candidateRoots() 里的 appDir/../tools 正好命中（appDir 是 bin/）。
# 改这里的 DESTINATION，就要同步改 Toolchain.cpp。
# ======================================================================

include(GNUInstallDirs)

install(TARGETS Syncine
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}   # Linux/Windows：<prefix>/bin/Syncine
        BUNDLE DESTINATION .                          # macOS：Syncine.app
)

# 随包分发的命令行工具（ffmpeg/ffprobe、yt-dlp）。
# 用 EXISTS 包住：开发机没跑过 scripts/fetch-tools.sh 时也要能配置成功。
foreach(tool_dir ffmpeg yt-dlp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/${tool_dir}")
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/${tool_dir}"
                DESTINATION tools
                USE_SOURCE_PERMISSIONS   # 保住可执行位，否则解包后无法运行
                PATTERN "cookies.txt" EXCLUDE)   # 用户登录态，绝不进包
    endif()
endforeach()

# 工具目录的说明文档（讲清来源、版本与许可）
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/README.md")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/tools/README.md" DESTINATION tools)
endif()

# ----------------------------------------------------------------------
# 14. Qt 运行库部署
#
# 用户机器上没装 Qt，必须把 Qt 动态库、平台插件、QML 运行时模块一起带走，
# 否则一启动就白屏。这个脚本在 Linux 上拷 Qt 库，Windows 上调 windeployqt，
# macOS 上调 macdeployqt —— 一份配置三平台生效。
#
# 拷贝的是动态库，正好满足 Qt LGPLv3「允许用户替换 Qt」的要求。
# ----------------------------------------------------------------------

qt_generate_deploy_qml_app_script(
        TARGET Syncine
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})

# ======================================================================
# 15. CPack —— 生成可分发的压缩包
#
# include(CPack) 必须放在所有 install() 之后：它在被 include 的那一刻
# 对当前已有的安装规则拍快照，放前面会漏装东西。
# ======================================================================

set(CPACK_PACKAGE_NAME "Syncine")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "DongGu")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "基于 WebRTC 的同步观影软件")
set(CPACK_PACKAGE_CONTACT "DongGuZhengHuaJi")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/DongGuZhengHuaJi/Syncine")

# 解包后就是一棵可以直接运行的目录树，不需要装到系统里
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_PACKAGE_FILE_NAME
        "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}")

if(WIN32)
    set(CPACK_GENERATOR "ZIP")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    # TGZ 是 Linux 的惯例（保留权限与符号链接）；ZIP 是给 Windows 用户顺手解压用的
    set(CPACK_GENERATOR "TGZ;ZIP")
endif()

include(CPack)
