# Syncine 发布手册（Release Handbook）

> 本手册面向「把 Syncine 从源码变成一个用户能直接下载运行、并且能跨平台的 1.0 版本」这件事。
> 每一节都按 **【作用】→【怎么做】→【注意事项】** 的结构写，目的是让你理解流程而不只是照抄命令。

---

## 目录

- [第 0 章 心智模型：发布软件的四个层次](#第-0-章-心智模型发布软件的四个层次)
- [第 1 章 发版前的仓库整理](#第-1-章-发版前的仓库整理)
- [第 2 章 版本号与单一数据源](#第-2-章-版本号与单一数据源)
- [第 3 章 构建系统改造（CMake）](#第-3-章-构建系统改造cmake)
- [第 4 章 第三方依赖 WebRTC 的管理](#第-4-章-第三方依赖-webrtc-的管理)
- [第 5 章 随包工具 ffmpeg / yt-dlp 的管理](#第-5-章-随包工具-ffmpeg--yt-dlp-的管理)
- [第 6 章 打包：让用户能直接运行](#第-6-章-打包让用户能直接运行)
- [第 7 章 跨平台策略](#第-7-章-跨平台策略)
- [第 8 章 持续集成与自动发布（GitHub Actions）](#第-8-章-持续集成与自动发布github-actions)
- [第 9 章 正式发版流程](#第-9-章-正式发版流程)
- [第 10 章 发版检查清单](#第-10-章-发版检查清单)
- [附录 A 许可证与合规](#附录-a-许可证与合规)
- [附录 B 目录结构建议](#附录-b-目录结构建议)
- [附录 C 命令速查](#附录-c-命令速查)

---

## 第 0 章 心智模型：发布软件的四个层次

**【作用】**
把一个项目「发出去」其实是四件独立的事。把它们混在一起思考，就会觉得千头万绪。分开之后，每一层都有明确的目标和工具。

**【怎么做】**

| 层次 | 目标 | 主要工具 |
|---|---|---|
| ① 源码管理 | 决定哪些文件入库、如何打标签发版 | git / GitHub |
| ② 构建 | 从源码生成可执行文件 | CMake + 编译器 |
| ③ 打包 | 把「可执行文件 + 运行所需一切」装进一个目录/文件 | CMake `install()` + CPack + Qt 部署工具 |
| ④ 分发 | 让用户拿到就能跑 | GitHub Release / AppImage / .deb / MSI / .dmg |

一个完整的发布流水线长这样：

```
源码 (git)
   │  ② 构建：cmake configure + build
   ▼
可执行文件 Syncine
   │  ③ 打包：install() 收集 + Qt 部署脚本拷依赖 + CPack 封装
   ▼
安装包 Syncine-1.0.0-Linux.tar.gz
   │  ④ 分发：git tag → GitHub Release 上传附件 → 用户下载
   ▼
用户直接运行
```

**【注意事项】**
- **跨平台 = 上面四层每一层都要按操作系统分别处理**，尤其 ②③ 层差别最大。
- 因此务必采用「**先在 Linux 上把整条链路走通，再逐个扩展平台**」的策略，不要一开始就三平台齐上。

---

## 第 1 章 发版前的仓库整理

**【作用】**
公开仓库有三类东西绝对不能进：**机密**（token 泄漏等于事故）、**大文件**（拖慢 clone、有配额）、**垃圾**（构建目录、日志）。同时要给项目补上「门面文件」，否则别人看不懂也用不了。

### 1.1 检查机密是否曾进入历史

**【作用】**
`.gitignore` 只管「以后」，管不了「以前」。如果某个密钥曾经被提交过，那它已经永久留在 git 历史里，删文件没用。

**【怎么做】**

```bash
# 检查这三类文件是否曾出现在任何历史提交里（无输出 = 干净）
git log --all --oneline -- Server/src/tools/Secrets.h
git log --all --oneline -- "STUN*TURN.txt"
git log --all --oneline -- Client/tools

# 也可以全库扫一遍可疑字符串
git log --all -S "TURN_API_TOKEN" --oneline
git log --all -S "1659061502" --oneline
```

**【注意事项】**
- **如果发现有输出**：处理顺序是「**先轮换 token，再清历史**」。
  1. 立刻去 Cloudflare 作废并重发那组 TURN token（因为历史里那份已经等同于公开）。
  2. 再用 `git filter-repo` 重写历史（破坏性操作，先备份、改完要 `git push --force`）。
- **密钥泄漏的正确处置是「轮换」，不是「删除」**——因为 fork、缓存、别人的本地副本都还会有。
- 本项目的 `.gitignore` 已经覆盖了 `Server/src/tools/Secrets.h`、`STUN*TURN.txt`、`Client/tools/*`、`logs`、`*/cmake-build-*`、`.idea`，这部分是好的。

### 1.2 检查是否有大文件拖慢仓库

**【作用】**
git 对大文件不友好：一旦提交，历史里永远有一份。clone 会变慢，仓库会膨胀。

**【怎么做】**

```bash
# 列出被跟踪文件里最大的几个
git ls-files | xargs -I{} du -h {} 2>/dev/null | sort -rh | head -20

# 看整个 .git 目录有多大（能反映历史里有没有大文件）
du -sh .git
```

**【注意事项】**
- 本项目的 `Client/tools/` 有约 **317 MB**（ffmpeg/ffprobe 各约 145 MB，yt-dlp 约 40 MB），已正确忽略。这些二进制应当**按需下载**，而不是入库（见第 5 章）。

### 1.3 补齐「门面文件」

**【作用】**
一个公开项目如果没有许可证和说明文档，别人在法律上不能使用你的代码，也不知道怎么构建。这些文件是「能不能被别人用起来」的前提。

**【怎么做】**

| 文件 | 作用 | 本项目状态 |
|---|---|---|
| `LICENSE` | 许可证；没有它默认「保留所有权利」，别人不能用 | ❌ 需要添加 |
| `README.md` | 项目介绍、截图、构建方法、依赖、使用方法 | ❌ 需要添加 |
| `CHANGELOG.md` | 版本变更记录，方便用户和未来的你 | 建议添加 |
| `.gitattributes` | 统一换行符、标记二进制/补丁文件 | 建议添加 |
| `.editorconfig` | 统一缩进风格（可选） | 可选 |
| `RELEASING.md` | 本手册 | 本文件 |

**【注意事项】**
- `README.md` 里必须写清楚本项目的三个真实门槛：
  1. 需要 **Qt ≥ 6.8**（`QAudioBufferOutput` 是 Qt 6.8 才引入，本项目实际用 6.10）；
  2. 需要 **WebRTC**（这是最大的门槛，见第 4 章）；
  3. **ffmpeg / yt-dlp 如何获取**（见第 5 章）。
- `.gitattributes` 示例：

```gitattributes
# 统一换行符
* text=auto

# 补丁文件必须用 LF，否则 Windows 上 git apply 会失败
*.patch text eol=lf

# 大二进制标记（避免 git 试图做文本 diff）
*.png binary
*.ico binary
*.icns binary
```

- 本项目 `docs/`、`design/` 目前在 `.gitignore` 里。如果那是想公开的设计文档/截图，应从 `.gitignore` 移出（README 里引用它会更有说服力）。

---

## 第 2 章 版本号与单一数据源

**【作用】**
版本号如果散落在多处（CMake、QML、代码、安装包、tag），迟早对不上。正确做法是**只在一处定义**，其余地方引用它。

**【怎么做】**

```cmake
# Client/CMakeLists.txt 顶部
project(Syncine VERSION 1.0.0 LANGUAGES CXX)
```

QML 模块直接引用：

```cmake
qt_add_qml_module(Syncine
    URI SyncineApp
    VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    ...
)
```

C++ 代码里想显示版本（比如「关于」对话框），生成一个头文件：

```cmake
# cmake/version.h.in
#pragma once
#define SYNCINE_VERSION "@PROJECT_VERSION@"
```

```cmake
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/version.h
    @ONLY
)
target_include_directories(Syncine PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
```

**【注意事项】**
- 建议遵循 **[语义化版本 SemVer](https://semver.org/lang/zh-CN/)**：`主版本.次版本.修订号`。
  - `1.0.0`：首个正式版；
  - `1.0.1`：只修 bug；
  - `1.1.0`：加功能但向后兼容；
  - `2.0.0`：不兼容的大改。
- CPack 打包时会读取 `PROJECT_VERSION`，所以它同时决定了安装包的版本号——单一数据源的价值就在这里。

---

## 第 3 章 构建系统改造（CMake）

**【作用】**
这是「从源码到可执行文件」的核心。改造目标有两个：
1. **可移植**——别人 clone 下来能用他自己的路径构建（去掉写死的 `/home/donggu/...`）；
2. **可打包**——通过 `install()` 规则描述「装什么、装到哪」，这是后续打包的基石。

### 3.1 去掉硬编码路径

**【作用】**
你现在 `Client/CMakeLists.txt` 写死了：

```cmake
set(WEBRTC_ROOT "/home/donggu/webrtc-checkout/src")   # 别人 clone 直接失败
```

`CMakePresets.json` 也写死了 `$HOME/Qt/6.10.2/gcc_64`。必须改成可被外部覆盖。

**【怎么做】**

```cmake
# WebRTC 目录：优先命令行 -DWEBRTC_ROOT=...，其次环境变量，都没有才报错
set(WEBRTC_ROOT "" CACHE PATH "WebRTC 源码目录（含 api/ 和 out/）")
if(NOT WEBRTC_ROOT AND DEFINED ENV{WEBRTC_ROOT})
    set(WEBRTC_ROOT "$ENV{WEBRTC_ROOT}")
endif()
if(NOT WEBRTC_ROOT)
    message(FATAL_ERROR
        "未指定 WebRTC 目录。请用 -DWEBRTC_ROOT=/path/to/webrtc-checkout/src，"
        "或设置环境变量 WEBRTC_ROOT。详见 RELEASING.md 第 4 章。")
endif()

set(WEBRTC_OUT_DIR "${WEBRTC_ROOT}/out/Default" CACHE PATH "WebRTC 构建输出目录")
```

**【注意事项】**
- `CACHE` 变量的意义：它会被记住（存在 `CMakeCache.txt` 里），也允许用 `-D` 覆盖。这正是「既给默认值、又让用户能改」的标准手段。
- Qt 路径同理：让用户通过 `CMAKE_PREFIX_PATH` 或 preset 覆盖，不要写死。

### 3.2 用 `find_package` 风格封装依赖（可选但推荐）

**【作用】**
把「找头文件 / 找库 / 找特殊链接器」的细节从主 `CMakeLists.txt` 里挪出去，主文件只写 `find_package(WebRTC REQUIRED)`，更清晰、也更容易换平台。

**【怎么做】**
建 `cmake/FindWebRTC.cmake`，把你现在的那套 `add_library(... IMPORTED)` 逻辑搬进去，主文件只留：

```cmake
find_package(WebRTC REQUIRED)
target_link_libraries(Syncine PRIVATE WebRTC::WebRTC)
```

**【注意事项】**
- 你现在的注释里记录了大量 ABI 相关细节（`-fno-rtti`、`NDEBUG`、CREL 重定位、Chromium 的 lld），**这些注释非常有价值，搬动时不要丢**。它们是「为什么这么写」的唯一线索。

### 3.3 加 `install()` 规则——打包的基石

**【作用】**
`install()` 是「构建」和「打包」之间的桥。**CPack 只会打包你 `install()` 出去的东西**。不写 `install()`，就没有安装包。

**【怎么做】**

```cmake
include(GNUInstallDirs)   # 定义 CMAKE_INSTALL_BINDIR = bin 等标准目录

install(TARGETS Syncine
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}   # Linux/Windows: <prefix>/bin/Syncine
    BUNDLE  DESTINATION .                          # macOS: Syncine.app
)

# 随包分发的工具：装到 <prefix>/tools
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg" DESTINATION tools)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/yt-dlp")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/yt-dlp" DESTINATION tools)
endif()
```

**【注意事项】**
- **安装布局必须和运行时查找路径对齐。** 你的 `Toolchain::candidateRoots()`（`Client/src/core/Toolchain.cpp`）已经在找：
  - `appDir/tools`
  - `appDir/../tools`
  - `AppDataLocation/bin`

  装成 `<prefix>/bin/Syncine` + `<prefix>/tools/ffmpeg/ffmpeg` 后，`appDir/../tools` 正好命中，**代码不用改**。如果你改了安装布局，就必须同步改 `candidateRoots()`。
- `install(TARGETS ...)` 必须在 `include(CPack)` **之前**（原因见 3.5）。

### 3.4 Qt 应用部署脚本

**【作用】**
Qt 程序运行需要一堆 **Qt 动态库**和**平台插件**（`libQt6Core.so`、`platforms/libqxcb.so`、QML 模块等）。用户机器上没装 Qt，所以必须把这些一起打包。手动拷贝极易漏。Qt 6.3+ 提供了标准机制，能自动搞定，且三平台通用。

**【怎么做】**

```cmake
qt_generate_deploy_qml_app_script(
    TARGET Syncine
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
```

**【注意事项】**
- 这个脚本在 Linux 上处理 Qt 库和 QML 插件，在 Windows 上调 `windeployqt`，在 macOS 上调 `macdeployqt`——**一份配置，三平台生效**。
- 你的 QML 已经通过 `qt_add_qml_module` 编进二进制资源了（不用单独发 `.qml`），但**QML 运行时模块**（QtQuick、Controls 等）仍需要这个脚本装进包里，否则用户机上一启动就白屏。
- 前提：需要 `qt_standard_project_setup()`（你已经有了）。

### 3.5 CPack——生成安装包

**【作用】**
CPack 读取 `install()` 规则，按你指定的格式生成 `.tar.gz` / `.deb` / `.zip` / `NSIS` / `.dmg` 等成品。

**【怎么做】**

```cmake
set(CPACK_PACKAGE_NAME "Syncine")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "DongGu")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "基于 WebRTC 的同步观影软件")
set(CPACK_PACKAGE_CONTACT "you@example.com")

if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ;DEB")
endif()

set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}")

include(CPack)   # 必须放在所有 install() 之后
```

构建产物：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release ...
cmake --build build
cd build && cpack           # 生成包
```

**【注意事项】**
- **`include(CPack)` 必须在最后**——它在被 `include` 的那一刻「快照」当前所有 `install()` 规则。放在前面会漏装东西。
- 发布版要用 **`Release`** 构建类型（你现在 preset 里是 `Debug`，需要加一个 release preset）。
- `.deb` 若依赖系统 Qt，要填 `CPACK_DEBIAN_PACKAGE_DEPENDS`；若把 Qt 全打包进去，则不需要。

### 3.6 构建 Presets

**【作用】**
Preset 把「一套完整的 configure/build 参数」存进仓库，别人一条命令就能复现你的构建环境。

**【怎么做】**
在 `Client/CMakePresets.json` 里增加 release 配置（保留现有 debug 配置）：

```json
{
  "configurePresets": [
    {
      "name": "release",
      "inherits": "qt-6.10",
      "binaryDir": "${sourceDir}/build-release",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" }
    }
  ],
  "buildPresets": [
    { "name": "release", "configurePreset": "release" }
  ]
}
```

用法：`cmake --preset release && cmake --build --preset release`

**【注意事项】**
- 注意你 CMakePresets 里 Qt 路径用的是 `$env{HOME}/Qt/6.10.2/gcc_64`。别人 Qt 装在别处时会失败，README 要说明「改 preset 或传 `-DCMAKE_PREFIX_PATH`」。

---

## 第 4 章 第三方依赖 WebRTC 的管理

**【作用】**
WebRTC 是本项目最大的第三方依赖，也是**跨平台的真难点**。你在本地对它的源码做了 2 处修改。这一章解决「如何让这些修改可复现、可跨平台、且不污染你的仓库」。

### 4.1 核心原则

> **WebRTC 不进仓库。入库的是「固定 revision + 补丁 + 构建脚本」。**

理由：
- WebRTC 源码/历史极其庞大，永远不该进 git；
- 终端用户只用**编译产物**，不碰源码；
- 补丁 + 版本号 + 脚本能让任何人**一模一样地**重建它。

### 4.2 记录你已有的改动

**【现状】** 本地 WebRTC revision 为 `bf31ff6fff0088bbbce706720e6f22be6f5294fa`（2026-09-10），改动 2 个文件：

```diff
# ① rtc_base/ssl_stream_adapter.h —— 补全 std 限定
-      nullptr_t /*field_trials*/) {
+      std::nullptr_t /*field_trials*/) {

# ② BUILD.gn —— 给 libwebrtc 目标补两个依赖
+      "api/video_codecs:builtin_video_decoder_factory",
+      "api/video_codecs:builtin_video_encoder_factory",
```

**【怎么做：导出补丁】**

```bash
mkdir -p third_party/webrtc/patches

cd /home/donggu/webrtc-checkout/src
git diff --no-color > /home/donggu/CLionProjects/Syncine/third_party/webrtc/patches/0001-syncine-build-fixes.patch
```

**【注意事项】**
- `git diff` 只包含**已跟踪文件**的改动。如果你以后**新建**了文件，要先 `git add -N .` 再 diff，否则新文件抓不到。
- 建议按语义**拆成多个补丁**（一个源码修复、一个构建图），将来上游修好其中一个时便于单独删除。
- 补丁文件用 LF 换行（见 `.gitattributes`）。

### 4.3 钉死 revision

**【作用】**
补丁只在特定 revision 上能干净应用。WebRTC 的 `main` 每天变化，不钉版本的话补丁迟早失效。

**【怎么做】**
把 revision 写进构建脚本和 README：

```
WebRTC revision: bf31ff6fff0088bbbce706720e6f22be6f5294fa (2026-09-10)
```

### 4.4 可复现的构建脚本

**【作用】**
把「同步 → 打补丁 → GN 配置 → 编译」全流程脚本化，消除手工步骤。

**【怎么做】** `scripts/build-webrtc.sh`：

```bash
#!/usr/bin/env bash
set -euo pipefail

WEBRTC_REV="bf31ff6fff0088bbbce706720e6f22be6f5294fa"
PATCH_DIR="$(cd "$(dirname "$0")/.." && pwd)/third_party/webrtc/patches"
CHECKOUT="${WEBRTC_CHECKOUT:-$HOME/webrtc-checkout}"

# 1) 同步到指定 revision（首次几 GB，很慢）
cd "$CHECKOUT"
gclient sync --revision "src@${WEBRTC_REV}"

# 2) 应用本地补丁（先 --check 验证能干净应用）
cd src
for p in "$PATCH_DIR"/*.patch; do
    git apply --check "$p"
    git apply "$p"
done

# 3) GN 配置（参数必须与客户端 CMake 的宏严格一致，见注意事项）
gn gen out/Default --args='
  is_debug=false
  rtc_include_tests=false
  rtc_build_examples=false
  rtc_enable_vp9=true
  treat_warnings_as_errors=false
'

# 4) 编译
ninja -C out/Default

echo "完成：$CHECKOUT/src/out/Default/obj/libwebrtc.a"
```

**【注意事项】**
- **GN 参数必须和 `Client/CMakeLists.txt` 里的编译宏对齐**。你注释里那段 `NDEBUG` 的教训（`sizeof(webrtc::Thread)` 256 vs 312、启动即段错误）就是参数不一致造成的。所以这些参数要写死在脚本里、并抄进 README，**绝不能靠记忆**。
- 把脚本里的 GN 参数换成**你实际构建时用的那一套**（上面的 `--args` 只是骨架示例）。

### 4.5 分发「预编译产物」而非源码树

**【作用】**
没人愿意为了用你的软件等几个小时的 WebRTC 编译。发布时应提供编译好的 `libwebrtc` 产物（解压后可能 1~2 GB），让你自己和 CI 直接复用。

**【怎么做】**
- 把编好的 WebRTC 产物（`libwebrtc.a` + 头文件 + 必要的 `gen/` 目录）打成归档，作为 **GitHub Release 附件** 或放对象存储；
- 客户端 CMake 提供「用预编译产物」的入口：

```cmake
# 二选一：
#   开发：-DWEBRTC_ROOT=/path/to/webrtc-checkout/src
#   发布：-DWEBRTC_PREBUILT=/path/to/webrtc-prebuilt
set(WEBRTC_PREBUILT "" CACHE PATH "预编译的 WebRTC 目录（含 lib 和 include）")
```

**【注意事项】**
- 预编译产物**绝不能进 git**（体积巨大）。用 Release 附件是标准做法。
- 跨平台时，每个平台各有一份预编译产物，命名里带上平台（如 `webrtc-linux-x64-bf31ff6f.tar.zst`）。

### 4.6 何时可以丢弃补丁

**【作用】**
减少长期维护负担。上游可能迟早修好你打的补丁。

**【怎么做】**

```bash
cd src && git apply --check <patch>   # 报错=上游改了这块，需人工合并
```

**【注意事项】**
- **1.0 阶段不要追 `main`**。钉死 `bf31ff6f`，冻结依赖，把精力放在打包上。
- 升级 WebRTC 的时机，放在「1.0 稳定之后」再规划。

---

## 第 5 章 随包工具 ffmpeg / yt-dlp 的管理

**【作用】**
Syncine 依赖外部的 `ffmpeg`/`ffprobe`（合并音视频、修时间戳、HDR 检查）和 `yt-dlp`（下载网络资源）。这些不是系统必装的，所以要**随包分发**。但它们体积大（约 317 MB），**不能进 git**。

### 5.1 用脚本下载，而不是入库

**【作用】**
让「获取工具」变成一条可重复执行的命令，任何人都能得到正确版本。

**【怎么做】** `scripts/fetch-tools.sh`（思路）：

```bash
#!/usr/bin/env bash
set -euo pipefail

DEST="$(cd "$(dirname "$0")/.." && pwd)/Client/tools"
mkdir -p "$DEST/ffmpeg" "$DEST/yt-dlp"

OS="$(uname -s)"

# --- yt-dlp ---
case "$OS" in
  Linux)   YTDLP="yt-dlp_linux" ;;
  Darwin)  YTDLP="yt-dlp_macos" ;;
  *)       YTDLP="yt-dlp.exe" ;;   # Windows
esac
curl -L -o "$DEST/yt-dlp/$YTDLP" \
     "https://github.com/yt-dlp/yt-dlp/releases/latest/download/$YTDLP"

# --- ffmpeg / ffprobe ---
# 从可信来源下载对应平台构建，并校验哈希（此处为示意，请填真实地址与哈希）
# ...

chmod +x "$DEST/yt-dlp/"* "$DEST/ffmpeg/"* 2>/dev/null || true
echo "工具已就绪：$DEST"
```

**【注意事项】**
- **务必固定版本并校验哈希**（`sha256sum -c`）。用「latest」有被投毒和被上游改坏的风险。
- 下载源要选可信的（ffmpeg 官方、gyan.dev、BtbN 等；yt-dlp 官方 GitHub Release）。
- 在 README 里写清楚：「运行 `./scripts/fetch-tools.sh` 获取工具」，否则用户 clone 下来功能残缺。

### 5.2 平台差异

**【作用】**
三个平台的二进制文件名不同，需要分别处理。

| | Linux | Windows | macOS |
|---|---|---|---|
| yt-dlp | `yt-dlp_linux` | `yt-dlp.exe` | `yt-dlp_macos` |
| ffmpeg | `ffmpeg` | `ffmpeg.exe` | `ffmpeg` |
| ffprobe | `ffprobe` | `ffprobe.exe` | `ffprobe` |

**【注意事项】**
- 好消息：你的 `Toolchain::findTool` 已经写了**多名字搜索**（`{"yt-dlp_linux","yt-dlp"}`），设计上已支持跨平台。只要按平台放对文件、放进正确目录（`tools/ffmpeg/`、`tools/yt-dlp/`）即可。

### 5.3 打包时带上它们

**【作用】**
`install()` 时把工具目录一并装进包里（见第 3.3 节的 `install(DIRECTORY ...)`）。

**【注意事项】**
- 用 `if(EXISTS ...)` 包住，保证「开发机没下工具」时也能配置成功。
- 记得把 ffmpeg 的 `LICENSE.txt` 一起打进去（见附录 A）。

---

## 第 6 章 打包：让用户能直接运行

**【作用】**
「能直接运行」的本质 = **可执行文件 + 运行时需要的一切**（Qt 库、插件、QML 模块、随包工具、运行库）都在包里，且**用户机器上没装 Qt 也能跑**。

### 6.1 Linux 三条路线

| 方案 | 产物 | 优点 | 缺点 |
|---|---|---|---|
| 便携压缩包 | `.tar.gz` | 最简单，CPack 直接出 | 用户要解压、手动运行 |
| 系统包 | `.deb` / `.rpm` | 集成进系统、有菜单 | 依赖系统 Qt，或需全打包 |
| **AppImage** | 单个 `.AppImage` | **下载→`chmod +x`→双击**，最符合「直接运行」 | 需额外工具链 |

**【AppImage 思路】**
1. 用 `cmake --install build --prefix AppDir/usr` 把东西装进 `AppDir/`；
2. 用 `linuxdeploy` + `linuxdeploy-plugin-qt` 补齐 Qt 依赖；
3. 用 `appimagetool` 打成单个 `.AppImage`。

**【注意事项】**
- **Linux 的 glibc 兼容性坑**：glibc 向前兼容、不向后兼容——在新系统上编的程序，老系统跑不了。若要覆盖尽量多用户，**在你能接受的最老发行版上构建**（例如在 Ubuntu 20.04 容器里编）。

### 6.2 Windows

**【作用】**
生成绿色 ZIP 或安装向导。

**【怎么做】**
- Qt 部署脚本会自动调用 `windeployqt`，把 `Qt6Core.dll` 等塞进目录；
- CPack 生成 `ZIP`（绿色版）或 `NSIS`（安装向导）。

**【注意事项】**
- 需要带上 **VC++ 运行库**，或改用静态链接 CRT。
- 二进制要考虑 32/64 位；现代项目基本只发 x64。

### 6.3 macOS

**【作用】**
生成标准 `.app` 和 `.dmg`。

**【怎么做】**
- `macdeployqt` 把 Qt 框架拷进 `Syncine.app`（由你第 3.4 节的部署脚本自动调用）；
- CPack 的 `DragNDrop` 生成 `.dmg`。

**【注意事项】**
- **必须做代码签名 + 公证（notarization）**，否则用户双击会被 Gatekeeper 拦下。这一步需要 Apple 开发者账号。
- 需要考虑 arm64 / x86_64，或做 universal binary。

### 6.4 验证打包结果（重要）

**【作用】**
本机装了 Qt，程序能跑；但用户机器没装。**必须在一个干净环境里验证**。

**【怎么做】**
- Linux：在干净的 Docker 容器里解包运行；用 `ldd Syncine` 检查是否有「not found」。
- Windows：在没装 Qt 的机器/虚拟机上跑。
- macOS：在干净用户账户下跑（验证签名）。

**【注意事项】**
- 常见的「本机能跑、用户不能跑」原因：漏拷 QML 模块、漏拷平台插件、`rpath` 指向了开发机的绝对路径。
- Linux 上确保可执行文件的 `RPATH` 指向 `$ORIGIN`（相对路径），而不是 `/home/donggu/Qt/...`。

---

## 第 7 章 跨平台策略

**【作用】**
明确「跨平台」的难度分布，避免一开始就陷进最深的坑。

### 7.1 难度分布

| 部分 | 跨平台难度 | 说明 |
|---|---|---|
| Qt/QML 界面代码 | 低 | Qt 本身就是跨平台的 |
| 业务逻辑（session/playback 等） | 低 | 纯 C++ |
| **Server** | 中低 | Boost + OpenSSL 跨平台，但依赖安装要解决 |
| 构建脚本（CMake） | 中 | 需处理平台宏、系统库、链接器差异 |
| **WebRTC** | **高** | 每平台都要单独构建，ABI/工具链约束苛刻 |

### 7.2 WebRTC 的三种应对方式

| 方案 | 说明 | 代价 |
|---|---|---|
| A. 每平台自编 WebRTC（推荐） | 固定 revision + 补丁 + 各平台构建脚本 | 工作量大，CI 编译数小时 |
| B. 用第三方预编译 WebRTC | 省去编译 | 版本/功能未必满足，风险高 |
| C. 抽象一层 WebRTC 接口 | 架构更干净 | 1.0 阶段不划算 |

**【注意事项】**
- 你现在的 `Client/CMakeLists.txt` 有多个**Linux 专属**写法：写死 `clang`、`-fuse-ld=lld`、`WEBRTC_LINUX`、链接 `X11/Xcomposite/...`、以及指向 Chromium 的 lld（CREL 重定位要求）。跨平台时这些都要按平台条件化（`if(WIN32) / if(APPLE)`）。
- **推荐路径**：1.0 只发 Linux → 把 WebRTC 构建脚本化 → 再逐个加 Windows/macOS。

### 7.3 Server 的跨平台依赖

**【作用】**
Server 用 Boost + OpenSSL，Windows/macOS 上默认没有，需要一套依赖获取方式。

**【怎么做】**
- 推荐 **vcpkg** 或 **conan** 管理 Boost/OpenSSL；或在 CMake 里用 `FetchContent`。
- Server 的 `cmake_minimum_required(VERSION 4.3)` **过于激进**——很多 CI 镜像/用户没有那么新的 CMake。建议降到与实际相符的版本（如 3.28），并与 Client 保持一致。
- `include_directories()` 建议改为 `target_include_directories(Server PRIVATE ...)`，避免全局污染。

**【注意事项】**
- Server 也要加 `install(TARGETS Server RUNTIME DESTINATION bin)`，否则无法打包分发。

---

## 第 8 章 持续集成与自动发布（GitHub Actions）

**【作用】**
把「构建 + 打包」自动化：每次打 tag，GitHub 自动在指定系统上构建、打包、上传到 Release。让人工只负责「打 tag 和写更新日志」。

### 8.1 工作流骨架

**【怎么做】** `.github/workflows/release.yml`：

```yaml
name: release

on:
  push:
    tags: ['v*']          # 只在推 v1.0.0 这类标签时触发

jobs:
  build:
    strategy:
      matrix:
        os: [ubuntu-latest]   # 先只 Linux，跑通再加 windows-latest / macos-latest
    runs-on: ${{ matrix.os }}
    steps:
      - uses: actions/checkout@v4

      - name: 安装 Qt
        uses: jurplel/install-qt-action@v4
        with:
          version: '6.10.2'
          modules: 'qtmultimedia qtwebsockets'

      - name: 下载随包工具
        run: ./scripts/fetch-tools.sh

      - name: 下载预编译 WebRTC
        run: ./scripts/fetch-webrtc.sh   # 从 Release 附件下载并解压

      - name: 配置
        run: cmake -B build -DCMAKE_BUILD_TYPE=Release -DWEBRTC_PREBUILT=$PWD/webrtc-prebuilt

      - name: 构建
        run: cmake --build build --parallel

      - name: 打包
        working-directory: build
        run: cpack

      - name: 上传到 Release
        uses: softprops/action-gh-release@v2
        with:
          files: build/*.tar.gz
```

**【注意事项】**
- **WebRTC 在 CI 里编译要几小时**，通常不现编，而是**提前编好当附件/缓存**，CI 只下载解压。上面的 `fetch-webrtc.sh` 就是这个用途。
- 用 `actions/cache` 缓存 Qt、WebRTC 等大依赖，能显著加速。
- tag 触发（`on: push: tags`）是标准做法：打 tag = 宣布要发版。

### 8.2 更进一步

**【作用】**
让发布更省心。

**【怎么做/注意事项】**
- 加一个「只在 PR 上跑、不发布」的 CI（`build + test`），保证合入的代码不会坏。
- 用 GitHub 的 **Environments** 保护 Release 权限。
- 未来可考虑自动更新（客户端检查新版本并提示）。

---

## 第 9 章 正式发版流程

**【作用】**
把前面所有准备串成一次可重复的「发版动作」。

**【怎么做】**

1. **确认代码冻结**：`main` 分支功能完成、CI 通过、没有已知阻塞 bug。
2. **更新版本文档**：写好 `CHANGELOG.md`（本次改了什么）。
3. **确认版本号**：`project(... VERSION 1.0.0)` 已改好。
4. **打标签**：

   ```bash
   git checkout main
   git pull
   git tag -a v1.0.0 -m "Syncine 1.0.0"
   git push origin main
   git push origin v1.0.0
   ```

5. **触发 CI**：推 tag 后 GitHub Actions 自动构建 + 打包 + 上传附件。
6. **在 GitHub 上建 Release**：选择 tag `v1.0.0`，标题 `Syncine 1.0.0`，正文粘贴更新日志；确认附件齐全（各平台包、校验和）。
7. **发布后验证**：下载自己发布的包，在干净环境跑一遍。
8. **如需修补**：发 `1.0.1`（只改 bug），不要偷偷覆盖 `1.0.0` 的附件。

**【注意事项】**
- **不要用分支/标签以外的方式发版**。tag 是「这一刻的代码快照」，必须和发布的二进制一一对应。
- **产物要提供校验和**（`sha256sum`），方便用户验证下载完整性。
- 如果 `1.0.0` 已经发出去又被发现问题，正确做法是发 `1.0.1`，而不是删掉重发——因为可能已经有人下载了。

---

## 第 10 章 发版检查清单

**【作用】**
发版前逐项打勾，避免遗漏。

```
[ ] 仓库历史里没有机密（Secrets.h / STUN⁄TURN.txt / Client/tools）
[ ] 如果有，token 已轮换 + 历史已清理
[ ] .gitignore 覆盖了 tools / build / logs / .idea
[ ] 有 LICENSE
[ ] README.md 写清了依赖、构建步骤、如何获取 ffmpeg/yt-dlp/WebRTC
[ ] .gitattributes 已加（补丁用 LF）
[ ] 版本号单一来源于 project(... VERSION)
[ ] CMake 无写死的 /home/donggu 路径
[ ] install() 规则齐全（可执行文件 + tools/）
[ ] qt_generate_deploy_qml_app_script + install(SCRIPT ...) 已加
[ ] include(CPack) 在所有 install() 之后
[ ] WebRTC：revision 已钉死、补丁已入库、构建脚本可用、GN 参数已记录
[ ] ffmpeg/yt-dlp：有 fetch 脚本 + 版本固定 + 哈希校验
[ ] 在干净环境验证过打包产物能运行
[ ] Server 也能构建 + 能 install
[ ] CHANGELOG 已更新
[ ] tag 已打、CI 已通过、Release 附件齐全、附了校验和
[ ] 许可证与第三方声明齐全（见附录 A）
```

---

## 附录 A 许可证与合规

**【作用】**
公开分发软件时，许可证不是可选项，处理不当会有法律风险。

**【怎么做/注意事项】**

| 组件 | 许可证 | 你要做的 |
|---|---|---|
| Qt | LGPLv3（默认） | **动态链接**发行即可满足 LGPL；若改成静态链接 Qt，必须用商业许可或 GPL |
| WebRTC | BSD-3-Clause | 保留版权声明；允许修改再分发（你已用补丁规范记录） |
| ffmpeg | LGPL 或 GPL（取决于构建） | 注意你下的构建是哪一种；随包附 `LICENSE.txt`；GPL 会传染整个分发 |
| yt-dlp | Unlicense | 注明即可 |
| Boost | Boost Software License | 宽松，注明即可 |
| OpenSSL | Apache-2.0 | 注明即可 |

- 使用 `qt_generate_deploy_qml_app_script` 拷贝 Qt **动态库**，正好符合 LGPL 的「允许用户替换 Qt」要求。
- 建议在 README 或 `LICENSES/` 目录里集中列出第三方组件的许可。

---

## 附录 B 目录结构建议

**【作用】**
清晰的结构让「源码 / 补丁 / 脚本 / 许可」各归其位。

```
Syncine/
├── README.md
├── RELEASING.md                 # 本手册
├── LICENSE
├── CHANGELOG.md
├── .gitattributes
├── .gitignore
├── cmake/
│   ├── FindWebRTC.cmake
│   └── version.h.in
├── scripts/
│   ├── build-webrtc.sh          # 维护者用：从源码构建 WebRTC
│   ├── fetch-webrtc.sh          # 开发/CI 用：下载预编译 WebRTC
│   └── fetch-tools.sh           # 下载 ffmpeg / yt-dlp
├── third_party/
│   └── webrtc/
│       ├── README.md            # 版本号 + GN 参数 + 补丁说明
│       └── patches/
│           └── 0001-syncine-build-fixes.patch
├── Client/
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── src/  ui/
│   └── tools/                   # 不入库，由 fetch-tools.sh 生成
└── Server/
    └── CMakeLists.txt
```

---

## 附录 C 命令速查

```bash
# ---- 仓库体检 ----
git log --all --oneline -- <path>          # 文件是否进过历史
git ls-files | xargs du -h | sort -rh | head   # 最大的被跟踪文件

# ---- 导出 WebRTC 补丁 ----
cd /home/donggu/webrtc-checkout/src
git diff --no-color > .../patches/0001-xxx.patch
git apply --check .../patches/0001-xxx.patch   # 验证能否干净应用

# ---- 构建与打包 ----
cmake --preset release
cmake --build --preset release
cd build-release && cpack

# ---- 安装到指定目录（模拟打包内容）----
cmake --install build-release --prefix /tmp/syncine-install
ldd /tmp/syncine-install/bin/Syncine | grep "not found"   # 检查缺失依赖

# ---- 发版 ----
git tag -a v1.0.0 -m "Syncine 1.0.0"
git push origin main && git push origin v1.0.0
sha256sum *.tar.gz   # 生成校验和
```

---

*本手册会随项目演进持续更新。原则是：**能在仓库里用文件描述的，就不要只留在脑子里。***
