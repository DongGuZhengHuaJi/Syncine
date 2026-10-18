# ======================================================================
# FindWebRTC.cmake —— 定位一份已经编译好的 WebRTC 静态库并封装成
#                   IMPORTED 目标 WebRTC::WebRTC
#
# 本项目不重新编译 WebRTC。这个模块只负责告诉 CMake 三件事：
#   头文件在哪里、libwebrtc.a 在哪里、链接时需要哪些额外开关。
#
# 输入（按优先级从高到低）：
#   1. -DWEBRTC_ROOT=<webrtc-checkout/src>
#   2. 环境变量 WEBRTC_ROOT
#   3. -DWEBRTC_OUT_DIR=<构建输出目录>，默认 <WEBRTC_ROOT>/out/Default
#
# 获得该目录的方式见 third_party/webrtc/README.md（自建或下载预编译产物）。
# ======================================================================

# ----------------------------------------------------------------------
# 1. 解析路径
# ----------------------------------------------------------------------

set(WEBRTC_ROOT "" CACHE PATH "WebRTC 源码目录（含 api/ 与 out/）")

if(NOT WEBRTC_ROOT AND DEFINED ENV{WEBRTC_ROOT})
    set(WEBRTC_ROOT "$ENV{WEBRTC_ROOT}")
endif()

if(NOT WEBRTC_ROOT)
    message(FATAL_ERROR
            "未指定 WebRTC 目录。\n"
            "请用 -DWEBRTC_ROOT=/path/to/webrtc-checkout/src 配置，\n"
            "或设置环境变量 WEBRTC_ROOT。\n"
            "获取方式见 third_party/webrtc/README.md。")
endif()

set(WEBRTC_OUT_DIR "${WEBRTC_ROOT}/out/Default" CACHE PATH "WebRTC 构建输出目录")

# 静态库文件名按平台不同：Linux 是 out/Default/obj/libwebrtc.a；
# Windows 上是 out/Default/obj/webrtc.lib（Chromium 在 Windows 上把整个 :webrtc
# 目标打成单个归档，不叫 libwebrtc）。
if(WIN32)
    set(WEBRTC_LIBRARY "${WEBRTC_OUT_DIR}/obj/webrtc.lib")
else()
    set(WEBRTC_LIBRARY "${WEBRTC_OUT_DIR}/obj/libwebrtc.a")
endif()

message(STATUS "WebRTC source : ${WEBRTC_ROOT}")
message(STATUS "WebRTC output : ${WEBRTC_OUT_DIR}")
message(STATUS "WebRTC library: ${WEBRTC_LIBRARY}")

# ----------------------------------------------------------------------
# 2. 平台约束
#
# 下面这套宏、系统库和链接器都是从一份 Linux 构建的 libwebrtc.a 反推出来的。
# Windows 走另一套（见第 5、6、7 节的 WIN32 分支），macOS 还没做。
# ----------------------------------------------------------------------

if(APPLE OR (NOT UNIX AND NOT WIN32))
    message(FATAL_ERROR
            "当前只支持 Linux 与 Windows：libwebrtc 必须在目标平台上构建"
            "（Windows 上 Chromium 的构建系统要求 MSVC）。\n"
            "移植到 macOS 需要按 README 的「跨平台」一节重出静态库并补齐宏与链接库。")
endif()

# ----------------------------------------------------------------------
# 3. 校验静态库存在
# ----------------------------------------------------------------------

if(NOT EXISTS "${WEBRTC_LIBRARY}")
    message(FATAL_ERROR
            "找不到 WebRTC 静态库：\n"
            "  ${WEBRTC_LIBRARY}\n"
            "WEBRTC_ROOT 指向的是源码目录，其 out/Default/ 里应当有构建产物。")
endif()

# ----------------------------------------------------------------------
# 4. 创建 IMPORTED 目标
# ----------------------------------------------------------------------

add_library(WebRTC::WebRTC STATIC IMPORTED GLOBAL)

set_target_properties(WebRTC::WebRTC PROPERTIES

        IMPORTED_LOCATION "${WEBRTC_LIBRARY}"

        # 头文件根目录。这样：
        #     #include "api/peer_connection_interface.h"
        # 就会从 <WEBRTC_ROOT>/api/... 查找。
        #
        # 后四个是 WebRTC 构建时生成/带入的第三方依赖，头文件也在其中：
        #   out/Default/gen            —— 生成的 protobuf/工厂声明等
        #   third_party/abseil-cpp     —— absl::AnyInvocable 等
        #   third_party/perfetto       —— 跟踪宏
        #   third_party/libyuv         —— 视频格式转换
        INTERFACE_INCLUDE_DIRECTORIES
        "${WEBRTC_ROOT};${WEBRTC_OUT_DIR}/gen;${WEBRTC_ROOT}/third_party/abseil-cpp;${WEBRTC_ROOT}/third_party/perfetto/include;${WEBRTC_OUT_DIR}/gen/third_party/perfetto;${WEBRTC_OUT_DIR}/gen/third_party/perfetto/build_config;${WEBRTC_ROOT}/third_party/libyuv/include"
)

# ----------------------------------------------------------------------
# 5. WebRTC 编译宏
#
# 这一节不是「随便加的开关」。下面每个宏都必须和编译 libwebrtc.a 时用的
# 那一套完全一致 —— WebRTC 头文件里有大量 `#if <宏>` 包裹的条件成员，
# 一旦不一致，同一个类在库里和客户端里 sizeof 就不同，内存布局错位。
#
# NDEBUG 就是这么踩出来的（症状：启动即 SIGSEGV）：
#   rtc_base/thread.h 里 webrtc::Thread 有 4 个成员被 `#if RTC_DCHECK_IS_ON`
#   包着，而 RTC_DCHECK_IS_ON = !defined(NDEBUG) || defined(DCHECK_ALWAYS_ON)。
#   libwebrtc.a 是 is_debug = false 编的（带 -DNDEBUG，RTC_DCHECK_IS_ON = 0），
#   客户端不带 NDEBUG 就是 1 —— 实测 sizeof(webrtc::Thread) 256 vs 312，
#   own_ss_ 偏移差 56 字节。于是 CreateWithSocketServer() 里
#   `own_ss_ = std::move(ss)` 读到的是垃圾指针，销毁旧值时崩在虚析构调用上。
#
# 所以这里 NDEBUG 的含义是「和库的 ABI 对齐」，不是「我要出发布版」。
# 它必须独立于 CMAKE_BUILD_TYPE：客户端哪怕切成 Debug 也照样得定义。
# ----------------------------------------------------------------------

target_compile_definitions(WebRTC::WebRTC INTERFACE
        NDEBUG # ABI 对齐用，勿删 —— 理由见上；WebrtcManager.cpp 有编译期护栏
        __STDC_CONSTANT_MACROS
        __STDC_FORMAT_MACROS
        ABSL_ALLOCATOR_NOTHROW=1
        RTC_ENABLE_VP9
        WEBRTC_HAVE_SCTP
        WEBRTC_ENABLE_PROTOBUF=1
        WEBRTC_STRICT_FIELD_TRIALS=0
)

if(WIN32)
    # Windows 版 webrtc.lib 是 Chromium 的 MSVC 工具链编的（is_debug=false，
    # 即 /MD + _ITERATOR_DEBUG_LEVEL=0）。这几个宏必须和它一致：
    #   WEBRTC_WIN            —— 选另一套条件编译分支（对应 Linux 的 WEBRTC_POSIX）
    #   NOMINMAX / WIN32_LEAN_AND_MEAN —— WebRTC 头文件期望的 Windows 头文件姿态
    #   _CRT_SECURE_NO_WARNINGS        —— 上游到处用 strncpy 之类
    #   _ITERATOR_DEBUG_LEVEL=0        —— 与 Release CRT 对齐；用 Debug（=2）链接会
    #                                     直接 LNK2038 mismatch（Qt 官方 debug 包是 /MDd）
    target_compile_definitions(WebRTC::WebRTC INTERFACE
            WEBRTC_WIN
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS
            _ITERATOR_DEBUG_LEVEL=0
    )
else()
    target_compile_definitions(WebRTC::WebRTC INTERFACE
            WEBRTC_POSIX
            WEBRTC_LINUX
            _FILE_OFFSET_BITS=64
            _GLIBCXX_ASSERTIONS=1
    )
endif()

# ----------------------------------------------------------------------
# 6. WebRTC 的系统依赖
#
# WebRTC 是个很大的静态库，链接期还会要求一批系统库。这里是当前明确需要
# 的那些；如果换版本后出现
#     undefined reference to ...
# 就按报错往这里补。
# ----------------------------------------------------------------------

if(WIN32)
    # Windows 的清单**不要手抄**：libwebrtc 依赖哪些系统库由 GN 决定，跟
    # revision 和 GN 参数（比如有没有 rtc_include_tests）一起变。正确做法是
    # 在编 webrtc.lib 的那台机器/CI 上跑：
    #     gn desc out/Default //:webrtc libs
    # 把输出（每行一个 xxx.lib）用分号连起来传给：
    #     -DWEBRTC_WIN_SYSTEM_LIBS="winmm.lib;ws2_32.lib;..."
    # 下面是没传时的兜底清单，够常见配置用；链接报 undefined 时优先用 gn desc 的结果。
    set(WEBRTC_WIN_SYSTEM_LIBS "" CACHE STRING
            "Windows 下 libwebrtc 需要的系统链接库（gn desc out/Default //:webrtc libs）")

    if(NOT WEBRTC_WIN_SYSTEM_LIBS)
        message(STATUS "WebRTC: 未指定 WEBRTC_WIN_SYSTEM_LIBS，使用兜底系统库清单")
        set(WEBRTC_WIN_SYSTEM_LIBS
                winmm.lib ws2_32.lib secur32.lib crypt32.lib iphlpapi.lib
                ole32.lib oleaut32.lib advapi32.lib shlwapi.lib user32.lib gdi32.lib
                msdmo.lib dmoguids.lib wmcodecdspuuid.lib strmiids.lib amstrmid.lib
                mfuuid.lib mfplat.lib mfreadwrite.lib d3d11.lib dxgi.lib
                shcore.lib dwmapi.lib authz.lib mscms.lib ksuser.lib bcrypt.lib)
    endif()

    target_link_libraries(WebRTC::WebRTC INTERFACE
            Threads::Threads
            ${WEBRTC_WIN_SYSTEM_LIBS}
    )

    # clang 编出来的 obj 会引用编译器内建函数，预编译产物随附了这份运行时。
    # 目录里带 clang 版本号（如 clang/24），用 glob 找，免得写死版本。
    file(GLOB WEBRTC_CLANG_RUNTIME
            "${WEBRTC_ROOT}/third_party/llvm-build/Release+Asserts/lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib")
    if(WEBRTC_CLANG_RUNTIME)
        target_link_libraries(WebRTC::WebRTC INTERFACE ${WEBRTC_CLANG_RUNTIME})
        message(STATUS "WebRTC clang runtime: ${WEBRTC_CLANG_RUNTIME}")
    else()
        message(WARNING
                "未找到 clang_rt.builtins-x86_64.lib；"
                "Windows 链接可能报编译器内建函数未定义。")
    endif()
else()
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
endif()

# ----------------------------------------------------------------------
# 7. 链接器：必须用认识 CREL 重定位的 lld（>= LLVM 19.1）
#
# libwebrtc.a 是用 Chromium 自带的 clang 编的，对象文件里用的是实验性的
# CREL 重定位格式 —— 节名是 `.crel.text` 而不是标准的 `.rela.text`。
#
# 系统自带的 lld 18 不认识 CREL —— 它不报错，只是把 `.crel.*` 当未知节静默
# 忽略。后果是库里所有重定位一个都没被施加：每个 call 的位移都是 0，调用会
# 「穿透」到紧随其后的下一条指令，把未初始化的寄存器当成函数返回值。
# 症状是程序启动即在 webrtc::Thread::CreateWithSocketServer() 里段错误，
# 而且 gdb 看到的是「往一个垃圾指针上做虚调用」，从代码层面完全看不出问题。
#
# 随 WebRTC 一起构建出来的 lld 就认识 CREL，直接指过去即可（它比 -fuse-ld
# 优先级高，所以系统里原有的 -fuse-ld=lld 会被覆盖）。
#
# 预编译产物若不附带 third_party/llvm-build，可用系统 lld ≥ 19.1 顶替：
# 通过 -DWEBRTC_LLD=/usr/bin/ld.lld 指定。
#
# Windows 上整节跳过：CREL 是 ELF 的格式，PE/COFF 没有这回事，用 MSVC 的
# link.exe（或 clang-cl 默认的 lld-link）即可。
# ----------------------------------------------------------------------

if(WIN32)
    message(STATUS "WebRTC: Windows/PE 无 CREL 重定位，不需要指定 lld")
else()
    set(WEBRTC_LLD
            "${WEBRTC_ROOT}/third_party/llvm-build/Release+Asserts/bin/ld.lld"
            CACHE FILEPATH "认识 CREL 重定位的 lld")

    if(NOT EXISTS "${WEBRTC_LLD}")
        message(FATAL_ERROR
                "找不到支持 CREL 重定位的链接器：\n"
                "  ${WEBRTC_LLD}\n"
                "libwebrtc.a 的对象文件使用 CREL 重定位，用系统 lld（< 19.1）链接不会报错，\n"
                "但会产出一个所有跨函数调用都失效的可执行文件（启动即段错误）。\n"
                "请改用 -DWEBRTC_LLD=<lld 路径> 指定一个 LLVM >= 19.1 的 ld.lld。")
    endif()

    # 挂在 WebRTC 这个 IMPORTED target 上，链接它的目标自动继承
    set_target_properties(WebRTC::WebRTC PROPERTIES
            INTERFACE_LINK_OPTIONS "--ld-path=${WEBRTC_LLD}"
    )
endif()
