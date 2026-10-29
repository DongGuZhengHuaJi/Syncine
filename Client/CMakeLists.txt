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

if(WIN32)
    # Windows 上不在这里挑编译器：能链 webrtc.lib 的只有 MSVC ABI 那一套
    # （Qt 的 win64_msvc2022_64 与 Chromium 出的静态库都是 MSVC ABI）。
    # 而且用 VS 生成器时 CMAKE_CXX_COMPILER 要到 project() 之后才有值，在这里
    # 判断会把「还没确定」误当成「没装 clang」直接 FATAL_ERROR。
    # 想用 clang-cl 就显式传 -DCMAKE_CXX_COMPILER=clang-cl（它同样认下面的 /GR-）。
elseif(NOT CMAKE_C_COMPILER OR NOT CMAKE_CXX_COMPILER)
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

project(Syncine VERSION 1.5.0 LANGUAGES CXX)

# WebRTC 使用 C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# gcc 14 对 WebRTC 头文件会误报 -Wchanges-meaning（clang 无此问题）
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-changes-meaning)
endif()

# 使用 lld（MSVC 的 link.exe 不认 -fuse-ld；clang-cl 走 lld-link，不需要这个选项）
if(NOT MSVC)
    add_link_options(-fuse-ld=lld)
endif()

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
# MSVC（含 clang-cl 的 MSVC 驱动模式）要写 /GR-，否则它会按默认 /GR 开 RTTI。
if(MSVC)
    add_compile_options(/GR-)
else()
    add_compile_options(-fno-rtti)
endif()

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
# 直播弹幕的 WebSocket 包体是 zlib 压缩的（实测即使 protover=0 也会发 ver=2）
find_package(ZLIB REQUIRED)

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

# 联调时用提交号区分“同为 1.5.0 但来自不同提交”的构建。
# CI 在 checkout 后配置，拿到的就是该 tag/commit 的哈希。
execute_process(
        COMMAND git rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE SYNCINE_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
)
if(NOT SYNCINE_GIT_COMMIT)
    set(SYNCINE_GIT_COMMIT "unknown")
endif()

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
        src/media/MediaPlatform.cpp
        src/media/MediaPlatform.h
        src/media/UrlInspector.cpp
        src/media/UrlInspector.h

        # danmaku：B 站弹幕 XML 的解析与按播放进度取条目
        src/danmaku/DanmakuController.cpp
        src/danmaku/DanmakuController.h
        src/danmaku/DanmakuXmlParser.cpp
        src/danmaku/DanmakuXmlParser.h
        # 直播弹幕：WebSocket + zlib 解包 + DANMU_MSG
        src/danmaku/LiveDanmakuSession.cpp
        src/danmaku/LiveDanmakuSession.h

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

        # transfer：文件传输（依赖 WebRTC 数据通道，不认识播放器）
        src/transfer/FileTransferModel.cpp
        src/transfer/FileTransferModel.h
        src/transfer/FileTransferManager.cpp
        src/transfer/FileTransferManager.h
        src/transfer/TransferChannels.h

        # webrtc
        src/webrtc/WebrtcManager.cpp
        src/webrtc/WebrtcManager.h
        src/webrtc/WebrtcLog.cpp
        src/webrtc/WebrtcLog.h
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
        "${CMAKE_CURRENT_SOURCE_DIR}/src/danmaku"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/media"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/playback"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/session"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/transfer"
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
        ui/components/FileTransferDialog.qml

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

        # 直播弹幕 WebSocket 的 zlib 包体
        ZLIB::ZLIB

        # WebRTC
        WebRTC::WebRTC
)

# ======================================================================
# 12.1 纯逻辑测试（不依赖 QML / 网络 / WebRTC 运行时）
#
# 只编译 FormatModel 与 Log，验证 spec 解释、档位/手选 id 优先级等纯规则。
# 不安装、不打包；CI 发版构建也会顺带编译，成本很低。
# ======================================================================

enable_testing()

add_executable(syncine_logic_tests
        tests/tst_formatmodel.cpp
        src/media/FormatModel.cpp
        src/core/Log.cpp
)

target_include_directories(syncine_logic_tests
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/media"
)

target_link_libraries(syncine_logic_tests PRIVATE Qt6::Core)

add_test(NAME formatmodel COMMAND syncine_logic_tests)

add_executable(syncine_platform_tests
        tests/tst_platform.cpp
        src/media/MediaPlatform.cpp
)

target_include_directories(syncine_platform_tests
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/media"
)

target_link_libraries(syncine_platform_tests PRIVATE Qt6::Core)

add_test(NAME platform COMMAND syncine_platform_tests)

add_executable(syncine_video_tests
        tests/tst_videosource.cpp
        src/webrtc/VideoTrackSource.cpp
        src/core/Log.cpp
)

target_include_directories(syncine_video_tests
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc"
)

target_link_libraries(syncine_video_tests
        PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Multimedia
        WebRTC::WebRTC
)

add_test(NAME videosource COMMAND syncine_video_tests)

# ======================================================================
# 12.2 PlaybackSync 加载时机测试
#
# 用 tests/fake_playback.cpp 替换 PlaybackController（记录每次 load/unload），
# 其余对象（房间、成员表、播放列表、协议）都是真实实现，验证：
#   · 成员表刷新不重建媒体，只有房主身份变化才重新决定播放源
#   · "已就绪"标记绑定实际播放源
# 不链接 WebRTC：播放器是替身，测试里没有远端渲染。
# ======================================================================

add_executable(syncine_playback_tests
        tests/tst_playbacksync.cpp
        tests/fake_playback.cpp
        tests/fake_playback.h

        src/playback/PlaybackSync.cpp
        src/playback/PlaybackSync.h
        # 只为让 AUTOMOC 生成它的元对象（方法体由 fake_playback.cpp 提供）
        src/playback/PlaybackController.h

        src/session/RoomSession.cpp
        src/session/RoomSession.h
        src/session/MemberModel.cpp
        src/session/MemberModel.h
        src/session/PlaylistModel.cpp
        src/session/PlaylistModel.h

        src/core/AppSettings.cpp
        src/core/AppSettings.h
        src/core/CookieManager.cpp
        src/core/CookieManager.h
        src/core/Log.cpp
        src/core/Log.h
        src/core/MediaHash.cpp
        src/core/MediaHash.h
        src/core/NetworkManager.cpp
        src/core/NetworkManager.h
        src/core/Protocol.cpp
        src/core/Protocol.h
        src/core/Toolchain.cpp
        src/core/Toolchain.h
        src/core/UrlDownloader.cpp
        src/core/UrlDownloader.h

        src/danmaku/DanmakuController.cpp
        src/danmaku/DanmakuController.h
        src/danmaku/DanmakuXmlParser.cpp
        src/danmaku/DanmakuXmlParser.h
        src/danmaku/LiveDanmakuSession.cpp
        src/danmaku/LiveDanmakuSession.h

        src/media/FormatModel.cpp
        src/media/FormatModel.h
        src/media/MediaPlatform.cpp
        src/media/MediaPlatform.h
)

target_include_directories(syncine_playback_tests
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/danmaku"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/media"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/playback"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/session"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc"
)

target_link_libraries(syncine_playback_tests
        PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Multimedia
        Qt6::Network
        Qt6::WebSockets
        ZLIB::ZLIB
)

add_test(NAME playbacksync COMMAND syncine_playback_tests)

# ======================================================================
# 12.3 媒体内存探针（验收工具）
#
# 用真实 PlaybackController 跑加载/解码/卸载循环，检查卸载后不保留最后一帧、
# 以及 RSS 不随轮次持续增长。需要 SYNCINE_MEMORY_VIDEO 指向一段真实视频，
# 没设置时测试自己跳过（详见 tests/tst_media_memory.cpp 顶部说明）。
#
# 只借用 WebRTC 的头文件，不链接 libwebrtc.a：PlaybackController 对
# WebrtcManager 的引用由 tests/stub_webrtcmanager.cpp 补上两个空实现。
# ======================================================================

get_target_property(SYNCINE_WEBRTC_INCLUDE_DIRS WebRTC::WebRTC
        INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(SYNCINE_WEBRTC_COMPILE_DEFS WebRTC::WebRTC
        INTERFACE_COMPILE_DEFINITIONS)

add_library(syncine_webrtc_headers INTERFACE)
target_include_directories(syncine_webrtc_headers
        INTERFACE ${SYNCINE_WEBRTC_INCLUDE_DIRS})
if(SYNCINE_WEBRTC_COMPILE_DEFS)
    target_compile_definitions(syncine_webrtc_headers
            INTERFACE ${SYNCINE_WEBRTC_COMPILE_DEFS})
endif()

add_executable(syncine_memory_probe
        tests/tst_media_memory.cpp
        tests/stub_webrtcmanager.cpp

        src/playback/PlaybackController.cpp
        src/playback/PlaybackController.h
        src/core/Log.cpp
        src/core/Log.h
        src/core/MediaHash.cpp
        src/core/MediaHash.h
)

target_include_directories(syncine_memory_probe
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/playback"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc"
)

target_link_libraries(syncine_memory_probe
        PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Multimedia
        syncine_webrtc_headers
)

add_test(NAME media_memory COMMAND syncine_memory_probe)
set_tests_properties(media_memory PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# ======================================================================
# 12.4 共享模式开关的发送侧内存探针
#
# 两条真实 PeerLink 在本进程内回环连通，真实 VideoTrackSource 推 4K 帧，
# 反复执行 main.cpp 的"停止/恢复共享"，逐轮记录 RSS。用来定位编码器在
# 开关共享模式时是否存在不回收的缓存。默认跳过，需显式开启：
#   SYNCINE_SHARE_MEMORY_PROBE=1 ./syncine_share_memory_probe
# ======================================================================

add_executable(syncine_share_memory_probe
        tests/tst_share_memory.cpp
        # 播放器模式下要真实解码；WebrtcManager 的两个符号用空实现补上
        tests/stub_webrtcmanager.cpp

        src/playback/PlaybackController.cpp
        src/playback/PlaybackController.h

        src/webrtc/PeerLink.cpp
        src/webrtc/PeerLink.h
        src/webrtc/VideoTrackSource.cpp
        src/webrtc/VideoTrackSource.h

        src/core/Log.cpp
        src/core/Log.h
        src/core/MediaHash.cpp
        src/core/MediaHash.h
)

target_include_directories(syncine_share_memory_probe
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/playback"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/transfer"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc"
)

target_link_libraries(syncine_share_memory_probe
        PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Multimedia
        WebRTC::WebRTC
)

add_test(NAME share_memory COMMAND syncine_share_memory_probe)

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
# Windows：固定 Qt Quick Controls 2 样式
#
# Windows 上 QQC2 的默认样式是原生 "Windows" 样式，它不支持覆盖
# background / contentItem —— 而 ui/ 下大量控件依赖这些覆盖来画自定义皮肤
# （运行时会打印 "The current style does not support customization..."，
# 控件退化成原生外观）。Linux 上默认就是非原生样式，所以这个问题只在
# Windows 上暴露。把 qtquickcontrols2.conf 放到可执行文件旁，Qt 启动时读取。
# ----------------------------------------------------------------------
if(WIN32)
    # Qt 6 从资源系统读取 :/qtquickcontrols2.conf，所以必须编译进可执行文件；
    # 同时也在 bin/ 旁边留一份，便于用户自己改。
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/qtquickcontrols2.conf"
            PROPERTIES QT_RESOURCE_ALIAS "qtquickcontrols2.conf")
    qt_add_resources(Syncine "qtquickcontrols2_conf"
            PREFIX "/"
            FILES "${CMAKE_CURRENT_SOURCE_DIR}/qtquickcontrols2.conf")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/qtquickcontrols2.conf"
            DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ----------------------------------------------------------------------
# Windows：随包分发 MSVC 运行库（CRT）
#
# 目标机器不一定装了 VC++ Redistributable，而本包是 /MD 动态链接 CRT 的。
# Linux 包把 Qt 之外的东西都带上了，Windows 包同样应当自包含。
# VCToolsRedistDir 由 vcvars64.bat 设置（形如 ...\VC\Redist\MSVC\<ver>\）。
# ----------------------------------------------------------------------
if(WIN32 AND DEFINED ENV{VCToolsRedistDir})
    # 必须转成正斜杠：路径里的 "\14.51..." 直接写进 cmake_install.cmake 会变成
    # 非法转义序列，打包时 CPack 报 "Syntax error in cmake code"。
    # 注意 TO_CMAKE_PATH 会吃掉结尾的反斜杠，所以拼接时必须自己补一个 "/"。
    file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" SYNCINE_REDIST_DIR)
    file(GLOB SYNCINE_CRT_DLLS
            "${SYNCINE_REDIST_DIR}/x64/Microsoft.VC*.CRT/*.dll")
    if(SYNCINE_CRT_DLLS)
        install(FILES ${SYNCINE_CRT_DLLS} DESTINATION ${CMAKE_INSTALL_BINDIR})
    else()
        message(WARNING
                "未找到 MSVC 运行库（VCToolsRedistDir=$ENV{VCToolsRedistDir}）："
                "打出的包需要目标机自行安装 VC++ Redistributable。")
    endif()
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
