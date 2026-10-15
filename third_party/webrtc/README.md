# WebRTC

Syncine 链接的是预编译静态库 `libwebrtc.a`，构建客户端时不编译 WebRTC。

本目录不包含 WebRTC 源码，只记录重建该静态库所需的全部信息：钉死的 revision、两份本地补丁、验证过的 GN 参数。

---

## 1. 版本

```
revision: bf31ff6fff0088bbbce706720e6f22be6f5294fa
日期    : 2026-09-10
平台    : linux-x86_64
```

1.0 阶段不追踪 `main`。WebRTC 的 `main` 持续变动，补丁只在上述 revision 上能干净应用；升级安排在 1.0 稳定之后。

---

## 2. 本地补丁

两份补丁位于 `patches/`，分开是为了将来上游修复其中之一时可以单独删除。

| 补丁 | 修改 | 原因 |
|---|---|---|
| `0001-ssl-stream-adapter-std-nullptr.patch` | `rtc_base/ssl_stream_adapter.h`：`nullptr_t` → `std::nullptr_t` | 上游缺少 `std` 限定，用 libstdc++ 编译报错 |
| `0002-build-gn-libwebrtc-video-codecs-deps.patch` | `BUILD.gn`：为 `libwebrtc` 目标补充两个依赖 | `builtin_video_decoder_factory` 与 `builtin_video_encoder_factory` 未列入依赖，链接期缺符号 |

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
| `use_custom_libcxx = false` | 使用系统 libstdc++。取 `true` 时 WebRTC 使用自带的 libc++，两侧的 `std::string` 等类型互不兼容 |
| `use_sysroot = false` | 使用系统头文件，不使用 Chromium 下载的 sysroot |
| `treat_warnings_as_errors = false` | 上游警告不阻断构建 |
| `rtc_include_tests = false` | 不编译测试目标 |
| `is_debug = false` | 决定是否定义 `NDEBUG`，见下 |

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

---

## 5. 预编译产物

分发给他人或供 CI 使用时，应提供预编译产物，避免使用方从源码重新构建。解压后约 900 MB。

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

---

## 6. 客户端如何引用

`cmake/FindWebRTC.cmake` 把该静态库封装为 CMake 的 IMPORTED 目标 `WebRTC::WebRTC`，客户端只需 `find_package(WebRTC REQUIRED)` 与 `target_link_libraries(... WebRTC::WebRTC)`。

该目标登记了四项内容：

| 内容 | 说明 |
|---|---|
| 静态库位置 | `<WEBRTC_OUT_DIR>/obj/libwebrtc.a` |
| 头文件目录 | 共 7 个：`WEBRTC_ROOT`、`out/Default/gen`、`third_party/abseil-cpp`、`third_party/perfetto/include`、`out/Default/gen/third_party/perfetto`、`out/Default/gen/third_party/perfetto/build_config`、`third_party/libyuv/include` |
| 编译宏 | 与第 3 节 GN 参数对齐的一组定义 |
| 链接选项 | `--ld-path=<WebRTC 自带的 lld>`，以及 WebRTC 依赖的系统库 |

因此预编译产物必须包含上述全部头文件目录，否则客户端无法编译。

---

## 7. 跨平台

`libwebrtc.a` 与平台相关，每个平台各需一份预编译产物。Windows 上 Chromium 的构建系统要求 MSVC，无法从 Linux 交叉编译，须在 Windows 机器上重新执行第 4 节的流程；两份补丁本身与平台无关。

客户端侧的平台相关配置集中在 `cmake/FindWebRTC.cmake`：系统库列表与 `WEBRTC_POSIX`、`WEBRTC_LINUX` 宏均为 Linux 专有，移植时需重新推导。
