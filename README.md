# Syncine

基于 WebRTC 的**同步观影**软件。开一个房间，和朋友一起看同一部影片——进度、暂停、快进都是同步的；想看同一个画面又不想上传片源时，房主的画面和电影音轨会经 WebRTC 直接推给其他人。

- **客户端**：Qt 6.10（QML）+ 原生 libwebrtc
- **服务端**：C++ / Boost.Asio + Beast，一个 WebSocket 房间服务器
- **跨平台**：Linux x86-64 与 Windows x86-64

---

## 三种观看模式

房间可以随时切换模式，**每种模式各存一份播放列表**，切回时仍在。

| 模式 | 谁有片源 | 怎么同步 |
|---|---|---|
| **同步模式** `local` | 每人各自持有同一个视频文件 | 只广播 play / pause / seek，各端播各自的文件。服务端比对内容哈希，有人在看别的版本会提示 |
| **共享模式** `share` | 只有房主有 | 房主的画面与电影音轨经 WebRTC 推给观众（观众不加载任何媒体）；观众不参与排片 |
| **网链模式** `url` | 各自从同一个链接下载 | 房间共享条目（链接 + 画质档位），各端各自解析、下载、合并再播放，房主广播位置供其他人纠偏 |

此外还有语音（麦克风）和聊天、本地弹幕。

---

## 快速开始

### 直接下载运行（Linux x86-64）

从 [Releases](https://github.com/DongGuZhengHuaJi/Syncine/releases) 下载 `Syncine-1.3.0-Linux.tar.gz`，解压后直接跑：

```bash
tar xzf Syncine-1.3.0-Linux.tar.gz
cd Syncine-1.3.0-Linux
./bin/Syncine
```

包里已经带了 Qt 运行库、平台插件、QML 模块，以及 `tools/` 下的 ffmpeg 与 yt-dlp，**不需要另外安装 Qt**。

**系统要求**：glibc ≥ 2.38（Ubuntu 24.04+ / Debian 13+ / Fedora 39+）。另外需要一整套桌面系统库——正常的图形界面发行版都已经有，若在精简环境里启动报缺库，装这些：

```bash
sudo apt install libgl1 libegl1 libglx0 libopengl0 libxkbcommon0 libxkbcommon-x11-0 \
  libfontconfig1 libfreetype6 libdbus-1-3 libgtk-3-0 libpulse0 libsndfile1 \
  libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 \
  libxcb-xkb1 libxcb-xfixes0 libxcb-shape0 libx11-xcb1 libxext6 libxrender1 \
  libxcomposite1 libxdamage1 libxrandr2 libxtst6
```

> `.zip` 是同一份内容，但 **Linux 用户建议用 `.tar.gz`**（zip 解压后可能丢失可执行位，且符号链接会变成普通文件）。
> 注意：Linux 包里那个 `.zip` 里装的是 Linux 程序，**不是** Windows 版；Windows 有独立的包，见下。

### 直接下载运行（Windows x86-64）

从 [Releases](https://github.com/DongGuZhengHuaJi/Syncine/releases) 下载 `Syncine-1.3.0-Windows.zip`，解压后直接跑：

```bat
Syncine-1.3.0-Windows\bin\Syncine.exe
```

包里已经带了 Qt 运行库、平台插件、QML 模块、`tools/` 下的 ffmpeg/ffprobe 与 yt-dlp，以及 **MSVC 运行库**——**不需要另外安装 Qt，也不需要装 Visual Studio 或 VC++ Redistributable**。

**系统要求**：Windows 10 / 11 x86-64。

### 连服务器

客户端默认连 `ws://8.148.12.233:8765`。也可以在「设置」里改成自己的服务器地址（`ws://` 或 `wss://`）。

---

## 从源码构建

### 客户端

**依赖**

| 依赖 | Linux | Windows |
|---|---|---|
| Qt | ≥ 6.8（本项目用 6.10.2）的 `gcc_64` | 同版本的 `win64_msvc2022_64`（含 `qtmultimedia`、`qtwebsockets`） |
| 编译器 | clang | **MSVC**（VS 2022 或 2026 均可） |
| 链接器 | lld ≥ 19.1 | 不需要，用 MSVC 自带的 `link.exe` |
| WebRTC | 见 [`third_party/webrtc/README.md`](third_party/webrtc/README.md) | 同上，**两个平台各有一份预编译产物** |
| ffmpeg / yt-dlp | 见 [`Client/tools/README.md`](Client/tools/README.md) | 同上（Windows 用 `yt-dlp.exe` 与 BtbN 的 `win64-lgpl` 构建） |

> 电影音轨用到了 `QAudioBufferOutput`，Qt 6.8 才引入，所以 Qt 不能低于 6.8。
> WebRTC 是按平台分别编译的：Linux 那份是 Chromium 的 clang 编的，**Windows 那份必须用 MSVC 编**——Chromium 在 Windows 上强制要求 MSVC，也无法从 Linux 交叉编译。

**Linux**

```bash
# 1. 随包工具（约 320 MB，不入仓库）
./scripts/fetch-tools.sh

# 2. WebRTC —— 二选一
#    a) 自己编译：见 third_party/webrtc/README.md
#    b) 下载预编译产物（Release 附件），解压到任意目录
export WEBRTC_ROOT=/path/to/webrtc-checkout/src

# 3. 构建
cmake --preset release          # 在 Client/ 目录下执行
cmake --build --preset release

# 4. 打包
cd build-release && cpack       # 出 Syncine-1.3.0-Linux.tar.gz 与 .zip
```

**Windows**

```bat
:: 0) 在「x64 Native Tools Command Prompt for VS」里执行（或先 call 一次 vcvars64.bat）
::    这样 cl.exe / link.exe / VCToolsRedistDir 才会在环境里

:: 1) 随包工具 → Client\tools\{ffmpeg,yt-dlp}
::    yt-dlp.exe 与 BtbN 的 ffmpeg-master-latest-win64-lgpl，见 Client/tools/README.md

:: 2) WebRTC：直接用预编译产物 webrtc-windows-x64-bf31ff6f.zip，解压到任意目录
::    （要自己从源码编，见 third_party\webrtc\README.md 第 7 节）

:: 3) 配置 + 构建
cmake -S Client -B Client\build-release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=<Qt>\6.10.2\msvc2022_64 ^
  -DWEBRTC_ROOT=<webrtc-windows-x64-bf31ff6f> ^
  "-DWEBRTC_WIN_SYSTEM_LIBS=<见 third_party\webrtc\README.md 第 7 节>"
cmake --build Client\build-release --parallel

:: 4) 打包（出 Syncine-1.3.0-Windows.zip）
cd Client\build-release
cpack
```

> Windows 上**不要用 `cmake --preset release`**：preset 里的 Qt 路径是 Linux 布局（`$env{HOME}/Qt/6.10.2/gcc_64`），Windows 要显式传 `-DCMAKE_PREFIX_PATH`。
> 另外 WebRTC 在 Windows 上不是「一个 `.a` 加一个 lld」就够，还需要一份 `clang_rt.builtins`，预编译产物里已经带了。

Qt 与 WebRTC 的位置都从**命令行参数/环境变量**读取，不写死在仓库里。Qt 不在默认位置时用 `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.2/<平台目录>` 覆盖。

**关于 CREL 重定位（仅 Linux）**：`libwebrtc.a` 的对象文件用的是 Chromium 的实验格式（节名 `.crel.text`）。系统自带的 lld < 19.1 不认识它——**不报错**，只是把那些节当未知节静默忽略，结果所有重定位都没被施加，产出启动即段错误、且从源码完全看不出问题的可执行文件。构建脚本会把随 WebRTC 一起编出来的那份 lld 指给链接器。

### 服务端

```bash
cd Server
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
./build-release/Server          # 监听 0.0.0.0:8765
```

依赖 Boost（Beast 是 header-only，只要头文件）、OpenSSL 与 nlohmann-json（Ubuntu 上为 `libboost-dev libssl-dev nlohmann-json3-dev`）。地址和端口在 `Server/src/tools/Config.h`。

**TURN 凭据**：服务端用 Cloudflare 的 TURN 密钥现场兑换临时凭据（有效期 24 小时）随入房消息下发，密钥只留在服务端。密钥来源按优先级：

1. 环境变量 `SYNCINE_TURN_KEY_ID` / `SYNCINE_TURN_API_TOKEN`
2. `Server/src/tools/Secrets.h`（**不入仓库**，见 `.gitignore`）

两者都没有时不报错，只下发公共 STUN——直连仍可用，NAT 穿透能力下降。

---

## 部署服务端

```bash
sudo install -m755 Server /opt/syncine/Server

sudo tee /etc/systemd/system/syncine.service >/dev/null <<'EOF'
[Unit]
Description=Syncine 房间服务器
After=network-online.target

[Service]
Type=simple
User=syncine
WorkingDirectory=/opt/syncine
Environment=SYNCINE_TURN_KEY_ID=...
Environment=SYNCINE_TURN_API_TOKEN=...
ExecStart=/opt/syncine/Server
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl enable --now syncine
```

- 日志在 `WorkingDirectory/logs/`，级别用 `SYNCINE_LOG_LEVEL`（trace/debug/info/warn/error）调。
- 需要放行 **8765/tcp**（云服务商的安全组和系统防火墙）。
- 服务端**不自带 TLS**。公网部署建议前面挂一层反向代理（nginx / caddy）做 `wss://` —— 否则房间口令、聊天内容和下发的 TURN 临时凭据都是明文。

---

## 跨平台

| 平台 | 状态 |
|---|---|
| Linux x86-64 | ✅ 1.0 支持 |
| Windows x86-64 | ✅ 1.0 支持。`webrtc.lib` 是在 Windows + MSVC 上从源码编出来的（Chromium 的构建系统在 Windows 上强制要求 MSVC，无法从 Linux 交叉编译），预编译产物为 `webrtc-windows-x64-bf31ff6f.zip` |
| macOS | ❌ 未验证 |

Windows 侧在**客户端**与 **WebRTC** 两边都做了平台适配（工具链与 SDK 版本、CRT、COM 套间、Qt Quick Controls 样式、随包运行库），逐条记录在 [`third_party/webrtc/README.md`](third_party/webrtc/README.md) 第 7 节。

---

## 已知限制（1.0）

- **信令未加密**：服务端是明文 `ws://`。房间口令、聊天、TURN 临时凭据可被同路径的中间人读到。媒体面走 WebRTC 自带加密（DTLS-SRTP），不受影响。有域名后在客户端改 `wss://` 并前置反代即可。
- **房间号只有 6 位**且入房失败没有节流，公网服务端存在枚举与口令爆破的可能。房间设个口令能挡掉一部分。
- **房间成员都能控制播放**（play / pause / seek 全员可发），这是「和朋友一起看」的设计，公网可能会受影响。
- 音画同步（lip sync）未实现；WebRTC 重协商逻辑不完整。
- 未做时钟漂移校准：位置同步是「动作广播 + 房主定期广播位置」，长时间播放后各端可能有秒级偏差。

---

## 许可证

本项目采用 [MIT](LICENSE)。

随包分发与链接的第三方组件：

| 组件 | 许可证 | 备注 |
|---|---|---|
| Qt 6 | LGPLv3 | **动态链接**分发，满足 LGPL 对「允许用户替换 Qt」的要求 |
| WebRTC | BSD-3-Clause | 本地补丁见 `third_party/webrtc/patches/`（含 Windows 的 COM 套间修复） |
| MSVC 运行库 | Microsoft 可再分发（VC++ Redistributable 许可） | 仅 Windows 包：`msvcp140.dll` / `vcruntime140.dll` 等随包分发，安装规则见 `Client/CMakeLists.txt` |
| ffmpeg | LGPLv3 | 用的是 BtbN 的 **lgpl** 构建（只做转封装，不需要 GPL 编码器）。`tools/ffmpeg/LICENSE.txt` |
| yt-dlp | Unlicense（源码） | 打好的可执行文件含 PyInstaller/GPLv3+ 组件，整份按 GPLv3+ 分发——属**聚合分发**，不影响本项目自身的许可 |
| Boost | Boost Software License | 仅服务端 |
| OpenSSL | Apache-2.0 | 仅服务端 |
