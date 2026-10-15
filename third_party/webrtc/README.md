# WebRTC（本项目最大的第三方依赖）

Syncine 用一个**预编译好的 `libwebrtc.a`**，不在构建时编译 WebRTC。

本目录不放 WebRTC 源码，只放「如何一模一样地重建它」的信息：钉死的版本号、两份补丁、以及确认过的 GN 参数。

---

## 1. 版本

```
revision: bf31ff6fff0088bbbce706720e6f22be6f5294fa
日期    : 2026-09-10
平台    : linux-x86_64
```

**1.0 阶段不要追 `main`。** WebRTC 的 main 每天在变，补丁只在钉死的 revision 上能干净应用。升级放到 1.0 稳定之后再做。

---

## 2. 本地补丁

两份补丁都在 `patches/`，分开放是为了将来上游修好其中一个时能单独删掉。

| 补丁 | 改什么 | 为什么 |
|---|---|---|
| `0001-ssl-stream-adapter-std-nullptr.patch` | `rtc_base/ssl_stream_adapter.h`：`nullptr_t` → `std::nullptr_t` | 上游漏了 `std` 限定，用 libstdc++ 编会报错 |
| `0002-build-gn-libwebrtc-video-codecs-deps.patch` | `BUILD.gn`：给 `libwebrtc` 补两个依赖 | `builtin_video_decoder_factory` / `builtin_video_encoder_factory` 没被列进依赖，链接期缺符号 |

应用方式：

```bash
cd <webrtc-checkout>/src
git apply --check /path/to/Syncine/third_party/webrtc/patches/*.patch
git apply /path/to/Syncine/third_party/webrtc/patches/*.patch
```

---

## 3. GN 参数（**必须逐字照抄**）

```gn
use_custom_libcxx = false
use_sysroot = false
treat_warnings_as_errors = false
rtc_include_tests = false
is_debug = false
```

`is_debug = false` 这一条尤其关键，它决定了 `-DNDEBUG`：WebRTC 头文件里大量 `#if RTC_DCHECK_IS_ON` 包裹的条件成员，而 `RTC_DCHECK_IS_ON = !defined(NDEBUG) || defined(DCHECK_ALWAYS_ON)`。

客户端必须带一致的宏，否则同一个类在库里和客户端里 `sizeof` 不同、内存布局错位。**实测**后果：`sizeof(webrtc::Thread)` 256 vs 312，`own_ss_` 偏移差 56 字节，程序启动即在 `CreateWithSocketServer()` 里段错误。所以 `cmake/FindWebRTC.cmake` 里把 `NDEBUG` 挂在 INTERFACE 上——它表示「和库的 ABI 对齐」，与客户端的 `CMAKE_BUILD_TYPE` 无关。

`use_custom_libcxx = false` 是为了和客户端的 libstdc++ ABI 兼容（否则 WebRTC 用自己那份 libc++，两边 `std::string` 都不是一个类型）。

---

## 4. 从源码构建

`scripts/build-webrtc.sh` 把「同步 → 打补丁 → GN → ninja」串成一条命令。首次同步是几 GB，编译要几小时。

```bash
WEBRTC_CHECKOUT=~/webrtc-checkout ./scripts/build-webrtc.sh
```

产物：`<checkout>/src/out/Default/obj/libwebrtc.a`，约 50 MB。

**顺带产出的链接器同样重要**：`<checkout>/src/third_party/llvm-build/Release+Asserts/bin/ld.lld`。`libwebrtc.a` 的对象文件用实验性的 **CREL 重定位**（节名 `.crel.text`，不是标准的 `.rela.text`），系统 lld < 19.1 不认识它——**不报错，只是把 `.crel.*` 当未知节静默忽略**，结果库里所有重定位都没被施加，产出一个「所有跨函数调用都失效」的可执行文件。必须用随 WebRTC 一起编出来的这个 lld。

---

## 5. 预编译产物（分发给他人 / CI 用）

没人愿意为了用你的软件等几小时编译。发布时应提供编好的产物。

打包内容（解压后约 1~2 GB）：

```
webrtc-linux-x64-bf31ff6f/
├── api/  rtc_base/  pc/  media/  modules/ ...   # 头文件（整个 src 树）
├── out/Default/
│   ├── obj/libwebrtc.a
│   └── gen/                                     # 生成的头文件，必需
└── third_party/llvm-build/Release+Asserts/bin/ld.lld
```

配置客户端时指向它即可：

```bash
cmake -DWEBRTC_ROOT=/path/to/webrtc-linux-x64-bf31ff6f ...
```

**产物绝不能进 git**（体积巨大），放 GitHub Release 附件或对象存储。若产物里的 `third_party/llvm-build` 被裁掉了，用 `-DWEBRTC_LLD=/usr/bin/ld.lld` 指一个 LLVM ≥ 19.1 的系统 lld。

---

## 6. 跨平台

`libwebrtc.a` 是平台相关的：**每个平台各有一份预编译产物**。Windows 上 Chromium 的构建系统强制要求 MSVC，无法从 Linux 交叉编译，必须在 Windows 机器上重新走一遍本文档的流程（含补丁，补丁本身与平台无关）。

客户端侧的平台条件也都集中在 `cmake/FindWebRTC.cmake`：系统库（X11 那一串）和 `WEBRTC_POSIX` / `WEBRTC_LINUX` 宏都是 Linux 专属的，换平台要重新推导。
