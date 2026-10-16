# WebRTC

Syncine 链接的是预编译静态库（Linux 上是 `libwebrtc.a`，Windows 上是 `webrtc.lib`），构建客户端时不编译 WebRTC。

本目录不包含 WebRTC 源码，只记录重建该静态库所需的全部信息：钉死的 revision、三份本地补丁、验证过的 GN 参数，以及 Windows 上额外需要的环境适配（见第 7 节）。

---

## 1. 版本

```
revision: bf31ff6fff0088bbbce706720e6f22be6f5294fa
日期    : 2026-09-10
平台    : linux-x86_64 / win-x86_64（同一 revision，各出一份产物）
```

1.0 阶段不追踪 `main`。WebRTC 的 `main` 持续变动，补丁只在上述 revision 上能干净应用；升级安排在 1.0 稳定之后。

两个平台用的是**同一个 revision、同一套补丁、同一套 GN 参数**；差别只在工具链（Linux 用 clang，Windows 用 MSVC）和少量环境适配，见第 7 节。

---

## 2. 本地补丁

三份补丁位于 `patches/`，分开是为了将来上游修复其中之一时可以单独删除。

| 补丁 | 平台 | 修改 | 原因 |
|---|---|---|---|
| `0001-ssl-stream-adapter-std-nullptr.patch` | 通用 | `rtc_base/ssl_stream_adapter.h`：`nullptr_t` → `std::nullptr_t` | 上游缺少 `std` 限定，用 libstdc++ 编译报错 |
| `0002-build-gn-libwebrtc-video-codecs-deps.patch` | 通用 | `BUILD.gn`：为 `libwebrtc` 目标补充两个依赖 | `builtin_video_decoder_factory` 与 `builtin_video_encoder_factory` 未列入依赖，链接期缺符号 |
| `0003-win-com-apartment.patch` | **仅 Windows** | `rtc_base/win/scoped_com_initializer.{h,cc}`：`RPC_E_CHANGED_MODE` 不再致命，并新增"引用是否由自己添加"的标记 | Qt 把 GUI 线程初始化为 STA，WebRTC 音频路径要在同一线程切 MTA。上游的致命 `RTC_CHECK_NE` 会让 Qt 应用启动即崩；只去掉断言又会让 `Succeeded()` 返回 false，导致音频设备模块创建失败。详见第 7.2 节 |

`0003` 修改的文件在非 Windows 平台上不参与编译，因此在 Linux 上应用它是安全的（`scripts/build-webrtc.sh` 的批量 `git apply` 无需特判）。

应用方式：

```bash
cd <webrtc-checkout>/src
git apply --check /path/to/Syncine/third_party/webrtc/patches/*.patch
git apply /path/to/Syncine/third_party/webrtc/patches/*.patch
```

---

## 3. GN 参数

必须与下表完全一致：

```gn
use_custom_libcxx = false
use_sysroot = false
treat_warnings_as_errors = false
rtc_include_tests = false
is_debug = false
```

| 参数 | 取值原因 |
|---|---|
| `use_custom_libcxx = false` | 使用系统标准库：Linux 上是 libstdc++，Windows 上是 MSVC STL。取 `true` 时 WebRTC 使用自带的 libc++，两侧的 `std::string` 等类型互不兼容 |
| `use_sysroot = false` | 使用系统头文件，不使用 Chromium 下载的 sysroot（Linux 概念，Windows 上无实际作用） |
| `treat_warnings_as_errors = false` | 上游警告不阻断构建 |
| `rtc_include_tests = false` | 不编译测试目标 |
| `is_debug = false` | 决定是否定义 `NDEBUG`，见下 |

Windows 上还要多一项（Linux 不需要）：

```gn
symbol_level = 0
```

原因：`build/config/compiler/compiler.gni` 对**非 Linux** 主机把 `default_symbol_level` 抬到 2，而 Linux release 默认就是 0 —— 这正是参考产物 `libwebrtc.a` 只有 49 MB 的原因。显式设为 0 让 Windows 与参考构建的**有效配置一致**，同时把编译时间与产物体积降下来。它不影响 `NDEBUG` / `_ITERATOR_DEBUG_LEVEL`，因此对 ABI 无影响。

### 3.1 `is_debug` 与 ABI 对齐

`is_debug = false` 使 WebRTC 带 `-DNDEBUG` 编译。WebRTC 头文件中有大量成员被 `#if RTC_DCHECK_IS_ON` 包裹，而：

```
RTC_DCHECK_IS_ON = !defined(NDEBUG) || defined(DCHECK_ALWAYS_ON)
```

两侧宏不一致时，同一个类在库内与客户端内的 `sizeof` 不同，内存布局错位。

**实测**：库为 `is_debug = false`（`RTC_DCHECK_IS_ON = 0`）、客户端未定义 `NDEBUG`（`RTC_DCHECK_IS_ON = 1`）时，`sizeof(webrtc::Thread)` 分别为 256 与 312，`own_ss_` 偏移相差 56 字节，程序在 `CreateWithSocketServer()` 中段错误。

因此 `cmake/FindWebRTC.cmake` 将 `NDEBUG` 挂在 INTERFACE 上。该宏表示 ABI 对齐，与客户端的 `CMAKE_BUILD_TYPE` 无关：客户端即使编 Debug 版也必须定义。

---

## 4. 从源码构建

`scripts/build-webrtc.sh` 依次执行同步、打补丁、GN 配置、ninja 编译。完整检出约 32 GB。

```bash
WEBRTC_CHECKOUT=~/webrtc-checkout ./scripts/build-webrtc.sh
```

### 4.1 产物

| 产物 | 路径 | 大小 |
|---|---|---|
| 静态库 | `<checkout>/src/out/Default/obj/libwebrtc.a` | 49 MB |
| 链接器 | `<checkout>/src/third_party/llvm-build/Release+Asserts/bin/ld.lld` | 310 MB |

链接器是必需项。`libwebrtc.a` 的目标文件使用实验性的 CREL 重定位格式（节名为 `.crel.text`，而非标准的 `.rela.text`）。系统 lld 低于 19.1 时不认识该格式，且**不报错**，只把 `.crel.*` 当作未知节静默忽略；结果是库内所有重定位都未被施加，产出的可执行文件所有跨函数调用失效。

### 4.2 Windows 上从源码构建

`scripts/build-webrtc.sh` 是 bash 脚本，Windows 上不能直接用。等价流程（在「x64 Native Tools Command Prompt for VS」里执行）：

```bat
set DEPOT_TOOLS_WIN_TOOLCHAIN=0
mkdir C:\w\webrtc-checkout && cd C:\w\webrtc-checkout
gclient config --name src --unmanaged https://webrtc.googlesource.com/src.git
gclient sync --revision src@bf31ff6fff0088bbbce706720e6f22be6f5294fa --no-history --nohooks
gclient runhooks
cd src
:: 0001 / 0002 / 0003
git apply <repo>\third_party\webrtc\patches\*.patch
gn gen out/Default --args="use_custom_libcxx=false use_sysroot=false treat_warnings_as_errors=false rtc_include_tests=false is_debug=false symbol_level=0"
ninja -C out/Default -j 8 webrtc
```

产物：`out/Default/obj/webrtc.lib`，约 81 MB。

Windows 专属的注意点：

| 事项 | 说明 |
|---|---|
| 必须设 `DEPOT_TOOLS_WIN_TOOLCHAIN=0` | 否则 gclient 会去下载 Google 内部的 Windows 工具链（需要授权，匿名访问会 401） |
| 工作目录用**短路径** | `LongPathsEnabled` 默认为 0（改注册表要管理员），Chromium 的目录层级很容易超过 260 字符 |
| `-j` 按内存给 | 20 逻辑核配 15.7 GB 内存时用 `-j 8`；全开有 OOM 风险 |
| 需要可用的 `python3` | `gn` 会调用它；若 PATH 上只有 Microsoft Store 的占位符，`gn gen` 报 `Returned 9009`。给自己装的 Python 复制一份 `python3.exe` 即可 |
| 改完 GN 参数必须重新 `gn gen` | 参数/构建配置变了要重新生成 `build.ninja`，否则 ninja 仍按旧配置编 |

**此外还要改 4 处 Chromium 构建配置**（涉及 3 个文件）。它们**不是**补丁文件，因为取的是「本机环境 vs Google 打包工具链」的差异，不同机器取值不同：

| 文件 | 上游值 | 本机值 | 原因 |
|---|---|---|---|
| `build/vs_toolchain.py` 的 `SDK_VERSION` | `10.0.28000.0` | 本机实际安装的版本，如 `10.0.26100.0` | 28000 是 Insider SDK，随 Google 打包工具链分发，非管理员装不了 |
| `build/toolchain/win/setup_toolchain.py` 的 `SDK_VERSION` | 同上 | 同上 | 两处必须一致，否则报 *Make sure the necessary SDK is installed* |
| `build/config/win/BUILD.gn` 的 `NTDDI_VERSION` | `NTDDI_WIN11_BR` | 本机 SDK 里存在的最高级别，如 `NTDDI_WIN11_GE` | `NTDDI_WIN11_BR` 只在新 SDK 的 `sdkddkver.h` 里定义 |
| `build/config/win/BUILD.gn` 的 `default_crt` | 桌面构建用 `:static_crt`（`/MT`） | 改成 `:dynamic_crt`（`/MD`） | **Qt 官方 Windows 包是 `/MD`**；混用会在客户端链接期报 `LNK2038` 加一片 `LNK2005` |

---

## 5. 预编译产物

分发给他人或供 CI 使用时，应提供预编译产物，避免使用方从源码重新构建。**每个平台各一份**，命名里带平台与 revision（如 `webrtc-linux-x64-bf31ff6f.tar.zst`、`webrtc-windows-x64-bf31ff6f.zip`）。

### 5.1 Linux

解压后约 900 MB。

```
webrtc-linux-x64-bf31ff6f/
├── api/  rtc_base/  pc/  media/  modules/ ...   头文件
├── out/Default/
│   ├── obj/libwebrtc.a
│   └── gen/                                     生成的头文件
└── third_party/llvm-build/Release+Asserts/bin/ld.lld
```

客户端按以下方式引用：

```bash
cmake -DWEBRTC_ROOT=/path/to/webrtc-linux-x64-bf31ff6f ...
```

产物不应进入 git，应作为 Release 附件或存放于对象存储。若产物中未包含 `third_party/llvm-build`，可用 `-DWEBRTC_LLD=/usr/bin/ld.lld` 指定系统 lld，版本须不低于 19.1。

### 5.2 Windows

解压后约 121 MB（压缩后约 30 MB）。

```
webrtc-windows-x64-bf31ff6f/
├── api/  rtc_base/  pc/  media/  modules/ ...   头文件
├── third_party/abseil-cpp/                      头文件
├── third_party/libyuv/include/                  头文件
├── third_party/perfetto/include/                头文件
├── third_party/llvm-build/Release+Asserts/lib/clang/24/lib/windows/
│   └── clang_rt.builtins-x86_64.lib             链接期必需（约 192 KB）
├── out/Default/
│   ├── obj/webrtc.lib                           静态库（约 81 MB）
│   └── gen/                                     生成的头文件
└── BUILD-INFO.md
```

引用方式与 Linux 相同，把 `WEBRTC_ROOT` 指向这个目录即可。

**与 Linux 产物的差异**：

| | Linux | Windows |
|---|---|---|
| 静态库 | `out/Default/obj/libwebrtc.a` | `out/Default/obj/webrtc.lib` |
| 链接器 | 必须附带 `ld.lld`（CREL 重定位，310 MB） | **不需要**——PE/COFF 没有 CREL 这回事，用 MSVC 的 `link.exe` |
| 额外必需文件 | —— | `clang_rt.builtins-x86_64.lib`：clang 编出来的 obj 会引用它的内建函数 |

**精简策略**：只保留编译和链接客户端真正需要的东西——头文件（`*.h` / `*.hpp` / `*.inc`）、生成的 `out/Default/gen`、静态库，以及那一个 192 KB 的 clang 运行时。源码（`*.cc`）、`.git`、测试与工具目录、只有编库才用得上的第三方源码全部剔除（原始检出约 32 GB）。

---

## 6. 客户端如何引用

`cmake/FindWebRTC.cmake` 把该静态库封装为 CMake 的 IMPORTED 目标 `WebRTC::WebRTC`，客户端只需 `find_package(WebRTC REQUIRED)` 与 `target_link_libraries(... WebRTC::WebRTC)`。

该目标登记了四项内容：

| 内容 | 说明 |
|---|---|
| 静态库位置 | Linux：`<WEBRTC_OUT_DIR>/obj/libwebrtc.a`；Windows：`<WEBRTC_OUT_DIR>/obj/webrtc.lib` |
| 头文件目录 | 共 7 个：`WEBRTC_ROOT`、`out/Default/gen`、`third_party/abseil-cpp`、`third_party/perfetto/include`、`out/Default/gen/third_party/perfetto`、`out/Default/gen/third_party/perfetto/build_config`、`third_party/libyuv/include` |
| 编译宏 | 与第 3 节 GN 参数对齐的一组定义（按平台分叉，见第 7.3 节） |
| 链接选项 | Linux：`--ld-path=<WebRTC 自带的 lld>` 加 X11 等系统库；Windows：`-DWEBRTC_WIN_SYSTEM_LIBS=...` |

因此预编译产物必须包含上述全部头文件目录，否则客户端无法编译。

---

## 7. 跨平台

### 7.1 现状

| 平台 | 状态 | 预编译产物 |
|---|---|---|
| Linux x86-64 | ✅ | `webrtc-linux-x64-bf31ff6f.tar.zst` |
| Windows x86-64 | ✅ | `webrtc-windows-x64-bf31ff6f.zip` |
| macOS | ❌ 未验证 | —— |

两个平台用**同一个 revision、同一套补丁、同一套 GN 参数**；差别只在工具链（Linux 用 clang，Windows 用 MSVC——Chromium 在 Windows 上强制要求 MSVC，也无法从 Linux 交叉编译）以及第 4.2 节列出的环境适配。

### 7.2 Windows 上两个必须处理的问题

这两点都是「Windows 独有」的坑，Linux 上永远碰不到。

**① CRT 必须与 Qt 一致（`/MD`）**

Chromium 在 `is_component_build=false` 时默认用**静态 CRT（`/MT`）**，而 Qt 官方 Windows 包是**动态 CRT（`/MD`）**。两者不能混：链接期会报 `LNK2038`（RuntimeLibrary 不匹配）加一片 `LNK2005`（同一个符号被静态/动态 CRT 各定义了一份）。所以第 4.2 节里必须把 `default_crt` 改成 `:dynamic_crt`。

**② COM 套间冲突（补丁 `0003`，必须打）**

Qt 的 Windows 平台插件会把 GUI 线程初始化为 **STA**（`OleInitialize`），而 WebRTC 的音频路径要在**同一线程**上切 **MTA**（`AudioDeviceWindowsCore::CoreAudioIsSupported()` 用 `ScopedCOMInitializer(kMTA)`）。上游把 `RPC_E_CHANGED_MODE` 当成致命错误：

```
# Fatal error in: ..\..\rtc_base\win\scoped_com_initializer.cc, line 43
# Check failed: ((HRESULT)0x80010106L) != hr_ (-2147417850 vs. -2147417850)
# Invalid COM thread model change (MTA->STA)
```

于是 Qt 应用在 GUI 线程上创建 `PeerConnectionFactory` 时**启动即崩**。

⚠️ **只删掉那个断言是不够的**。`Succeeded()` 的实现是 `SUCCEEDED(hr_)`，而 `CoreAudioIsSupported()` 里有：

```cpp
ScopedCOMInitializer comInit(ScopedCOMInitializer::kMTA);
if (!comInit.Succeeded()) {
  // Things will work even if an STA thread is calling this method but we
  // want to ensure that MTA is used and therefore return false here.
  return false;
}
```

断言去掉后 `hr_` 仍是失败码 → `Succeeded()` 为 false → `CreateAudioDeviceModule()` 返回 `nullptr` → **media 工厂建不起来，音视频轨全部不可用**（客户端日志只会看到「创建 voice 声卡失败，放弃初始化」，不再有任何报错，比崩溃更难发现）。

所以 `0003` 的做法是：遇到 `RPC_E_CHANGED_MODE` 时**报告成功**（COM 在本线程确实可用），同时记下「这个引用不是自己加的」，析构时不再 `CoUninitialize`——否则会误释放 Qt 的 COM 引用。上游在那句注释里本来就承认「STA 线程调用也能工作」。

### 7.3 客户端侧的平台差异

集中在 `cmake/FindWebRTC.cmake`：

| | Linux | Windows |
|---|---|---|
| 静态库名 | `obj/libwebrtc.a` | `obj/webrtc.lib` |
| 平台宏 | `WEBRTC_POSIX`、`WEBRTC_LINUX`、`_FILE_OFFSET_BITS=64`、`_GLIBCXX_ASSERTIONS=1` | `WEBRTC_WIN`、`NOMINMAX`、`WIN32_LEAN_AND_MEAN`、`_CRT_SECURE_NO_WARNINGS`、`_ITERATOR_DEBUG_LEVEL=0` |
| 系统库 | `X11`、`Xcomposite` 等 | `-DWEBRTC_WIN_SYSTEM_LIBS=...`，清单由 `gn desc out/Default //:webrtc libs --all-deps` 导出 |
| 链接器 | 必须指定带 CREL 支持的 `ld.lld` | 不需要；但要额外链 `clang_rt.builtins-x86_64.lib` |

`NDEBUG` 两个平台都必须定义（ABI 对齐，见第 3.1 节），与平台无关。
