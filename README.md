# Syncine

基于 WebRTC 的**同步观影**软件。开一个房间，和朋友一起看同一部影片——进度、暂停、快进都是同步的；想看同一个画面又不想上传片源时，房主的画面和电影音轨会经 WebRTC 直接推给其他人。

- **客户端**：Qt 6.10（QML）+ 原生 libwebrtc
- **服务端**：C++ / Boost.Asio + Beast，一个 WebSocket 房间服务器
- **跨平台**：Linux x86-64 与 Windows x86-64
- **当前版本**：1.3.0

---

## 功能

### 三种观看模式

房间可以随时切换模式，**每种模式各存一份播放列表**，切回时仍在。切换模式只有房主可以操作。

| 模式 | 谁有片源 | 怎么同步 |
|---|---|---|
| **同步模式** `local` | 每人各自持有同一个视频文件 | 广播 play / pause / seek / 倍速，各端播各自的文件。服务端比对内容哈希，有人在看别的版本会提示 |
| **共享模式** `share` | 只有房主有 | 房主的画面与电影音轨经 WebRTC 推给观众（观众不加载任何媒体）；观众不参与排片，也不支持倍速 |
| **网链模式** `url` | 各自从同一个链接下载 | 房间共享条目（链接 + 画质档位），各端各自解析、下载、合并再播放；房主定期广播位置，其他端偏差过大时自动对齐。支持倍速、直播和 B 站弹幕 |

### 语音、聊天与文件传输

- **语音**：麦克风推流，可在运行时切换麦克风/听筒设备，也可随时开关麦克风。
- **聊天**：房间消息同时作为弹幕显示；消息不落盘，退出房间即清空。
- **文件传输**：成员之间通过 WebRTC DataChannel 点对点传文件，不经过服务器。
  - 控制消息（确认、取消、校验、心跳）走独立的无序通道，文件块走有序通道；大文件传输不会再把控制消息堵在重传队列后面。
  - 分块传输（32KB）、进度/速度显示、双方确认、SHA-256 校验；接收端按 128KB/100ms 合并 ACK，发送端用 512KB 窗口限制在途数据。
  - 发送端按 SCTP 发送缓冲做二次节流，避免发送队列无限增长；15 秒没有进展会主动请求续传，两条通道都有 5 秒 ping / 15 秒超时健康检查。
  - 连接中断后 60 秒内可自动断点续传；超过窗口会失败，需要重新发送。新版本的数据通道名已改为 `syncine-file-ctl` / `syncine-file-data`，两端需要一起升级。
  - 发送窗口默认 2MB，可用环境变量 `SYNCINE_TRANSFER_WINDOW_KB` 调整（128–16384）。传输期间每 15 秒输出一条 `WebRTCStats` 诊断日志（candidate 类型、RTT、可用带宽、DataChannel 字节），进度日志每 10 秒一条。

### B 站弹幕

- **点播**：网链模式播放 B 站视频时，通过 yt-dlp 的 `danmaku` 字幕轨拉取 XML，按播放进度显示，并在条目目录缓存。
- **直播**：网链模式播放 B 站直播时，通过弹幕 WebSocket 实时接收；支持 Cookie 登录态认证、心跳和断线重连。
- **设置**：显示区域、不透明度、字号、速度、是否随播放器缩放。
- 聊天和弹幕共用同一份消息：发送的评论会同时进入评论列表和弹幕层。

### 断线重连与会话恢复

- 客户端信令断开后**保留房间**，不会立刻退回首页；顶部显示“正在重连…”。
- 客户端 10 秒心跳 / 15 秒 pong 超时；服务端也有 WebSocket 空闲检测，半开连接会被清理。
- 服务端在成员断线后保留 120 秒；客户端在 90 秒内自动 `resume`，恢复原 clientId、房主身份和 P2P 连接。
- 其他成员看到的是“连接中断，等待重连…”，而不是“退出又加入”；成员列表保留该成员并标记离线，顶栏只统计在线人数。
- 自动重连超过 90 秒后，加入房间页会提供“恢复上次会话”按钮，仍可手动恢复（前提是服务端 120 秒宽限还没过期）。
- ICE 失败会自动做 ICE restart，复用原 PeerConnection、轨道和 DataChannel。

> **客户端与服务端必须一起升级到 1.3.0 或更高。** 新客户端重连只走 `resume`，旧服务端会返回 `bad_message`。

### 播放与媒体

- 网链模式画质档位随条目保存，默认「最高画质」，可在设置里限制为不超过指定分辨率。
- HDR / 杜比视界检测：设置关闭时只选 SDR，并用 ffprobe 复核下载结果，发现 HDR 直接拒绝；不再做离线转码。
- 网链模式播放列表支持逐条重试：当前条目可重新解析/下载；其他条目在后台重试并写入缓存，不打断正在播放的条目。
- 同步模式和网链模式支持倍速；共享模式和直播不提供。
- 网链模式条目显示下载进度；画面/声音分离下载时按整体进度折算。
- 电影音量与语音音量可分别调节。
- 可从浏览器或 `cookies.txt` 文件导入 Cookie，供 yt-dlp 下载需要登录的站点。
- 随包携带 ffmpeg 与 yt-dlp，用户无需另外安装。

---

## 快速开始

### 直接下载运行（Linux x86-64）

从 [Releases](https://github.com/DongGuZhengHuaJi/Syncine/releases) 下载 `Syncine-1.3.0-Linux.tar.gz`，解压后直接运行：

```bash
tar xzf Syncine-1.3.0-Linux.tar.gz
cd Syncine-1.3.0-Linux
./bin/Syncine
```

包里已经带了 Qt 运行库、平台插件、QML 模块，以及 `tools/` 下的 ffmpeg 与 yt-dlp，**不需要另外安装 Qt**。

**系统要求**：glibc ≥ 2.38（Ubuntu 24.04+ / Debian 13+ / Fedora 39+）。正常的图形界面发行版通常已经具备所需桌面库；若在精简环境里启动报缺库，安装：

```bash
sudo apt install libgl1 libegl1 libglx0 libopengl0 libxkbcommon0 libxkbcommon-x11-0 \
  libfontconfig1 libfreetype6 libdbus-1-3 libgtk-3-0 libpulse0 libsndfile1 \
  libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 \
  libxcb-xkb1 libxcb-xfixes0 libxcb-shape0 libx11-xcb1 libxext6 libxrender1 \
  libxcomposite1 libxdamage1 libxrandr2 libxtst6
```

> `.zip` 是同一份内容，但 Linux 用户建议用 `.tar.gz`（zip 解压后可能丢失可执行位，符号链接也会变成普通文件）。
> Linux 包里的 `.zip` 装的是 Linux 程序，**不是** Windows 版。

### 直接下载运行（Windows x86-64）

下载 `Syncine-1.3.0-Windows.zip`，解压后运行：

```bat
Syncine-1.3.0-Windows\bin\Syncine.exe
```

包里已经带了 Qt 运行库、平台插件、QML 模块、`tools/` 下的 ffmpeg/ffprobe 与 yt-dlp，以及 **MSVC 运行库**——不需要另外安装 Qt、Visual Studio 或 VC++ Redistributable。

**系统要求**：Windows 10 / 11 x86-64。

### 服务端包

从 1.2.1 起，Release 同时提供 Linux 与 Windows 服务端包：

| 平台 | 文件 | 内容 |
|---|---|---|
| Linux x86-64 | `Syncine-Server-1.3.0-Linux-x86_64.tar.gz` | `bin/Server`、`LICENSE` |
| Windows x64 | `Syncine-Server-1.3.0-Windows-x64.zip` | `bin/Server.exe`、`LICENSE` |

Linux 包是动态链接的：目标机器需要 OpenSSL 运行库（`libssl` / `libcrypto`）；Boost 只用头文件组件（Asio/Beast、UUID/Random），不需要额外的 Boost 运行库。Windows 包使用 vcpkg 静态链接（静态 Boost/OpenSSL + `/MT` 运行库），单个 `Server.exe` 即可运行。

服务器默认监听 `0.0.0.0:8765`。

### 连服务器

客户端默认连：

```text
ws://8.148.12.233:8765
```

也可以在「设置」里改成自己的服务器地址（`ws://` 或 `wss://`）。修改地址后，客户端会按“用户地址 → 内置兜底地址”的顺序尝试连接；一旦连上，掉线重连会**固定回到同一台服务器**，不会误连到另一台。

---

## 使用说明

### 创建 / 加入房间

1. 首页选择「创建房间」，填写房间名、昵称和可选口令；
2. 把房间号发给好友；
3. 好友在「加入房间」页输入 6 位房间号、昵称和口令。

房间号是 6 位数字，口令可选，同一房间内昵称不能重复。服务端不会持久化房间，服务端重启后房间消失。

### 播放控制

- 进入房间后，同步模式的每位成员、共享模式的房主需要先指定本地视频文件（点画面或播放列表的「添加」）；网链模式直接添加链接。
- 同步模式和网链模式的点播内容要等所有成员都准备好（本地文件已加载 / 链接已解析完成）才能播放；直播不等待，共享模式也不需要等待，房主加载好片源即可开始。
- 房间内所有成员都可以 play / pause / seek（直播只能播放 / 暂停，进度条不可用）；这是「和朋友一起看」的设计。
- 倍速在同步模式和网链模式的点播中可用，房间内会同步。
- 共享模式的观众不加载本地媒体，画面和电影音轨由房主推流；play / pause / seek 仍会同步给房主执行，但共享模式不支持倍速。
- 同步 / 网链模式下成员都可以添加条目；删除和切集只有房主可以操作。共享模式只有房主能添加。

### 弹幕

- 点播弹幕会自动下载并缓存到条目目录；
- 直播弹幕通过 WebSocket 实时接收；
- 弹幕设置可从播放控制条的「弹幕设置」打开；
- 未登录的匿名直播连接可能被 B 站限流，建议导入 Cookie。

### 语音与文件传输

- 麦克风与听筒可在设置页的「音频」或播放控制条的「声音设置」中选择，修改立即生效；播放控制条上的麦克风按钮只控制本端是否发送语音。
- 文件传输入口在成员列表的邮件按钮；对方确认后开始，双方都可取消，连接中断后 60 秒内自动续传。

### 断线时会发生什么

| 情况 | 表现 |
|---|---|
| 短暂断开（服务端未检测到） | 客户端自动重连并 resume，其他人可能完全无感 |
| 服务端已标记离线，但 90 秒内恢复 | 其他人先看到“连接中断，等待重连…”，恢复后变回在线 |
| 超过 90 秒未恢复 | 客户端退回首页；加入房间页出现“恢复上次会话” |
| 服务端 120 秒宽限到期 | 成员被移除（普通成员广播离开；房主则解散房间） |
| 服务端重启 | 房间状态丢失，resume 返回 `room_not_found` |

---

## 从源码构建

### 客户端

**依赖**

| 依赖 | Linux | Windows |
|---|---|---|
| Qt | ≥ 6.8（本项目用 6.10.2）的 `gcc_64` | 同版本的 `win64_msvc2022_64`（含 `qtmultimedia`、`qtwebsockets`） |
| 编译器 | clang | **MSVC**：用预编译 `webrtc.lib` 时需要 VS 2026（14.51 STL）；自己重编 WebRTC 时 VS 2022 也可以 |
| 链接器 | lld ≥ 19.1 | MSVC 自带的 `link.exe` |
| zlib | `zlib1g-dev` | vcpkg 的 `zlib:x64-windows-static-md`（或等价的静态库） |
| WebRTC | 见 [`third_party/webrtc/README.md`](third_party/webrtc/README.md) | 同上，两个平台各有一份预编译产物 |
| ffmpeg / yt-dlp | 见 [`Client/tools/README.md`](Client/tools/README.md) | 同上（Windows 用 `yt-dlp.exe` 与 BtbN 的 `win64-lgpl` 构建） |

> 电影音轨用到 `QAudioBufferOutput`，Qt 6.8 才引入，所以 Qt 不能低于 6.8。
> WebRTC 按平台分别编译：Linux 那份是 Chromium 的 clang 编的，**Windows 那份必须用 MSVC 编**，无法从 Linux 交叉编译。

**Linux**

```bash
# 0. 基础依赖（以 Ubuntu 24.04 为例）
sudo apt install build-essential cmake ninja-build clang lld zlib1g-dev \
  curl zstd unzip \
  libgl1-mesa-dev libx11-dev libxcomposite-dev libxdamage-dev libxext-dev \
  libxfixes-dev libxrandr-dev libxrender-dev libxtst-dev

# 1. 随包工具（约 320 MB，不入仓库）
./scripts/fetch-tools.sh

# 2. WebRTC —— 二选一
#    a) 自己编译：见 third_party/webrtc/README.md
#    b) 下载预编译产物：./scripts/fetch-webrtc.sh <下载地址> [目标目录]
export WEBRTC_ROOT=/path/to/webrtc-checkout/src   # 预编译产物则指向解压出的 webrtc-linux-x64-* 目录

# 3. 构建（在 Client/ 目录下执行）
cd Client
cmake --preset release
cmake --build --preset release

# 4. 打包
cd build-release && cpack
# 产出 Syncine-1.3.0-Linux.tar.gz 与 .zip
```

> preset 里的 Qt 路径默认是 `~/Qt/6.10.2/gcc_64`。装在别处时在命令行覆盖：
> `cmake --preset release -DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.2/gcc_64`。
> `WEBRTC_ROOT` 同理，可 `export` 或用 `-D` 传入。

**Windows**

```bat
:: 0) 在「x64 Native Tools Command Prompt for VS 2026」里执行
::    预编译 webrtc.lib 由 VS 2026（MSVC 14.51）的 STL 编出，用 VS 2022 链接会缺 __std_* 符号
::    确保 cl.exe / link.exe / VCToolsRedistDir 在环境里

:: 1) 随包工具 → Client\tools\{ffmpeg,yt-dlp}
::    yt-dlp.exe 与 BtbN 的 ffmpeg-master-latest-win64-lgpl，见 Client/tools/README.md

:: 2) WebRTC：使用预编译产物 webrtc-windows-x64-bf31ff6f.zip
::    自己从源码编见 third_party\webrtc\README.md 第 4.2 节

:: 3) zlib（静态库 + /MD，和 Qt/WebRTC 的 CRT 一致）
vcpkg install zlib:x64-windows-static-md

:: 4) 配置 + 构建
cmake -S Client -B Client\build-release -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=<Qt>\6.10.2\msvc2022_64 ^
  -DZLIB_INCLUDE_DIR=<vcpkg>\installed\x64-windows-static-md\include ^
  -DZLIB_LIBRARY=<vcpkg>\installed\x64-windows-static-md\lib\libzs.lib ^
  -DWEBRTC_ROOT=<webrtc-windows-x64-bf31ff6f> ^
  "-DWEBRTC_WIN_SYSTEM_LIBS=<见 third_party\webrtc\README.md 第 7 节>"
cmake --build Client\build-release --parallel

:: 5) 打包（出 Syncine-1.3.0-Windows.zip）
cd Client\build-release
cpack
```

> Windows 上不要用 `cmake --preset release`：preset 里的 Qt 路径是 Linux 布局，Windows 要显式传 `-DCMAKE_PREFIX_PATH`。
> WebRTC 在 Windows 上还需要一份 `clang_rt.builtins`，预编译产物里已经带了。
> `WEBRTC_WIN_SYSTEM_LIBS` 不传时 `FindWebRTC.cmake` 有兜底系统库清单；链接报未定义符号时，再用 `gn desc out/Default //:webrtc libs` 的结果覆盖。
> 预编译的 `webrtc.lib` 必须用 VS 2026 链接；只有 VS 2022 时，需要按 [`third_party/webrtc/README.md`](third_party/webrtc/README.md) 第 4.2 节自己重编一份。
> zlib 静态库在 Windows 上通常叫 `libzs.lib`，如实际文件名不同以 `installed\<triplet>\lib` 下的文件为准。

**关于 CREL 重定位（仅 Linux）**：`libwebrtc.a` 的对象文件使用 Chromium 的实验格式（节名 `.crel.text`）。系统自带的 lld < 19.1 不认识这种节——**不报错**，只是静默忽略，结果重定位没有施加，产出启动即段错误、从源码完全看不出问题。构建脚本会把随 WebRTC 一起编出来的那份 lld 指给链接器。

### 服务端

**Linux**

```bash
sudo apt install build-essential cmake ninja-build \
  libboost-dev libssl-dev nlohmann-json3-dev

cd Server
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
./build-release/Server          # 监听 0.0.0.0:8765
```

**Windows（vcpkg）**

```bat
vcpkg install boost-asio boost-beast boost-uuid boost-random ^
  openssl nlohmann-json --triplet x64-windows-static

cmake -S Server -B Server\build-release -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>\scripts\buildsystems\vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build Server\build-release --config Release --parallel
```

> 使用 VS 2026 时把生成器换成 `"Visual Studio 18 2026"`，其余参数不变。

服务端依赖 Boost（Asio/Beast、UUID/Random 均为 header-only）、OpenSSL 与 nlohmann-json；CMake ≥ 3.28，C++23。地址、端口和配额在 `Server/src/tools/Config.h`。

**TURN 凭据**：服务端优先使用自建 coturn，未配置 coturn 时回退 Cloudflare；两者都不可用时只下发公共 STUN（直连仍可用，NAT 穿透能力下降）。凭据随入房消息下发，密钥只留在服务端。

- **自建 coturn（推荐）**：配置 `SYNCINE_COTURN_URLS` / `SYNCINE_COTURN_SECRET` / `SYNCINE_COTURN_STUN_URLS`（可选）/ `SYNCINE_COTURN_TTL`（可选）。coturn 使用 `use-auth-secret`，服务端本地用 HMAC-SHA1 生成短期 username/credential，不依赖外部 API。
- **Cloudflare（回退）**：只有在没有配置 coturn 时才使用。密钥来源：
  1. 环境变量 `SYNCINE_TURN_KEY_ID` / `SYNCINE_TURN_API_TOKEN`
  2. `Server/src/tools/Secrets.h`（**不入仓库**，见 `.gitignore`）

服务端自带的 `deploy/coturn/install-coturn.sh` 会安装并配置 coturn、写入 `/opt/syncine/env`，并保留原有的 Cloudflare 配置。删掉 `SYNCINE_COTURN_*` 几行并重启即可回到 Cloudflare。

---

## 部署服务端

以 Linux + systemd 为例：

```bash
# 1. 二进制（源码构建或解压 Release 里的服务端包）
sudo mkdir -p /opt/syncine
sudo install -m755 Server/build-release/Server /opt/syncine/Server
# 或者从 Syncine-Server-1.3.0-Linux-x86_64.tar.gz 解压后：
# sudo install -m755 bin/Server /opt/syncine/Server

# 2. 服务端专用用户
sudo useradd --system --no-create-home --shell /usr/sbin/nologin syncine || true
sudo chown -R syncine:syncine /opt/syncine

# 3. TURN 配置放环境文件，不要写进 unit
#    使用 coturn 时第 5 步会自动写入 SYNCINE_COTURN_*；
#    这里的 Cloudflare 密钥只作为未配置 coturn 时的回退，可留空。
sudo install -m600 /dev/null /opt/syncine/env
sudo tee /opt/syncine/env >/dev/null <<'EOF'
# 未配置 coturn 时才需要：
# SYNCINE_TURN_KEY_ID=...
# SYNCINE_TURN_API_TOKEN=...
# SYNCINE_LOG_LEVEL=info
EOF
sudo chown syncine:syncine /opt/syncine/env

# 4. systemd
sudo tee /etc/systemd/system/syncine.service >/dev/null <<'EOF'
[Unit]
Description=Syncine 房间服务器
After=network-online.target

[Service]
Type=simple
User=syncine
Group=syncine
WorkingDirectory=/opt/syncine
EnvironmentFile=-/opt/syncine/env
ExecStart=/opt/syncine/Server
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now syncine

# 5. coturn（推荐）：安装、写配置、写 env 并重启 syncine
#    Release 的服务端包已包含 deploy/coturn；源码构建时用 Server/deploy/coturn。
sudo SYNCINE_PUBLIC_IP=<公网IP> \
     bash /opt/syncine/deploy/coturn/install-coturn.sh
```

- coturn 需要放行 **3478/udp、3478/tcp、49160-49300/udp、49160-49300/tcp**（云安全组 + 系统防火墙）；脚本会尝试配置 ufw/firewalld，但云安全组需要手动放行。
- 脚本会把 `SYNCINE_COTURN_URLS` / `SYNCINE_COTURN_SECRET` 等写入 `/opt/syncine/env`，并保留原有的 Cloudflare 配置。
- 删除 `SYNCINE_COTURN_*` 四行并重启 `syncine`，即可回退到 Cloudflare TURN。
- coturn 的 `static-auth-secret` 与 `/opt/syncine/env` 里的 secret 必须一致；脚本会自动生成并写入两边。

- 日志在 `/opt/syncine/logs/`；`SYNCINE_LOG_LEVEL`（trace/debug/info/warn/error）和 `SYNCINE_LOG_DIR` 可覆盖。
- 需要放行 **8765/tcp**（云安全组和系统防火墙）。
- 服务端**不自带 TLS**。公网部署建议前置 nginx/caddy 做 `wss://`——否则房间口令、聊天内容和下发的 TURN 临时凭据都是明文。
- 房间状态只在内存里；服务端重启后所有房间消失。
- 成员断线后服务端会广播离线状态；其他客户端显示“连接中断，等待重连…”，120 秒内未恢复才真正移除成员。
- 配额在 `Server/src/tools/Config.h`：最多 500 个房间、每房间 16 人、每种模式 200 条播放列表、单条消息 64 KB。

---

## 跨平台

| 平台 | 状态 |
|---|---|
| Linux x86-64 | ✅ 支持 |
| Windows x86-64 | ✅ 支持。`webrtc.lib` 在 Windows + MSVC 上从源码编出，预编译产物为 `webrtc-windows-x64-bf31ff6f.zip` |
| macOS | ❌ 未支持 |

Windows 侧在**客户端**与 **WebRTC** 两边都做了平台适配（工具链、CRT、COM 套间、Qt Quick Controls 样式、随包运行库），详见 [`third_party/webrtc/README.md`](third_party/webrtc/README.md) 第 7 节。

---

## 项目结构

```text
Client/                 Qt 6 + QML 客户端
  src/core/             设置、日志、网络与信令、协议、Cookie、工具链、下载
  src/session/          房间状态、成员、播放列表、入场与断线恢复
  src/playback/         播放器封装与同步策略
  src/webrtc/           PeerConnection、语音/视频/电影音轨、信令
  src/media/            yt-dlp 格式解析与链接探测
  src/danmaku/          B 站点播 XML 弹幕与直播 WebSocket 弹幕
  src/transfer/         P2P 文件传输（含续传与拥塞控制）
  ui/pages/             首页、建房 / 加入 / 房间 / 设置页
  ui/components/        播放器、控制条、弹幕层、文件传输等组件
Server/                 Boost.Asio/Beast 房间服务器
  src/server/           服务进程启动与主循环
  src/network/          会话、房间、WebSocket、消息分发
  src/tools/            日志、TURN 凭据、配置
docs/                   协议、播放列表、直播设计与项目分析
scripts/                随包工具、WebRTC 下载/打包/构建脚本
cmake/                  FindWebRTC.cmake
third_party/webrtc/     WebRTC revision、补丁与构建说明
.github/workflows/      发布流水线
```

---

## 文档

- [通信协议 v1](docs/protocol.md)
- [播放列表设计](docs/playlist-design.md)
- [直播支持（网链模式）](docs/live-streams.md)
- [WebRTC 构建与平台适配](third_party/webrtc/README.md)
- [更新日志](CHANGELOG.md)
- [发布手册](RELEASING.md)

---

## 已知限制

- **信令未加密**：服务端默认是明文 `ws://`。房间口令、聊天和 TURN 临时凭据可能被同路径的中间人读到；媒体面有 WebRTC 的 DTLS-SRTP 加密。公网部署请使用 `wss://` 反代。
- **房间号只有 6 位**，入房失败没有节流，公网服务端存在枚举和口令爆破风险；建议设置口令，并在反代层加限速。
- **房间成员都能控制播放**（play / pause / seek；直播不可拖动进度，倍速仅在同步 / 网链模式的点播中可用）。
- **没有账号系统**：不区分用户，join 房间即可参与；服务端不持久化房间和聊天。
- **音画同步（lip sync）未实现**；未做时钟漂移校准，长时间播放后各端可能有秒级偏差。
- **房主掉线 120 秒后房间解散**；暂不支持房主转让。
- **文件传输续传窗口为 60 秒**；超过后需要重新发送。新版本使用双 DataChannel（控制/数据分离），与旧客户端的文件传输不互通，两端需要一起升级。
- **网链模式没有持久缓存**：退出软件后临时下载会被清理，下次观看会重新下载。
- **macOS 未支持**；Linux 包要求 glibc ≥ 2.38。
- **客户端和服务端需要一起升级**：1.3.0 的重连协议与旧服务端不兼容。

---

## 许可证

本项目采用 [MIT](LICENSE)。

随包分发或链接的第三方组件：

| 组件 | 许可证 | 备注 |
|---|---|---|
| Qt 6 | LGPLv3 | **动态链接**分发，满足 LGPL 对「允许用户替换 Qt」的要求 |
| WebRTC | BSD-3-Clause | 本地补丁见 `third_party/webrtc/patches/`（含 Windows 的 COM 套间修复） |
| MSVC 运行库 | Microsoft 可再分发（VC++ Redistributable 许可） | 仅 Windows 客户端包：`msvcp140.dll` / `vcruntime140.dll` 等随包分发 |
| ffmpeg | LGPLv3 | BtbN 的 **lgpl** 构建（只做转封装，不需要 GPL 编码器），许可证见 `Client/tools/ffmpeg/LICENSE.txt` |
| yt-dlp | Unlicense（源码） | 打好的可执行文件含 PyInstaller/GPLv3+ 组件，整份按 GPLv3+ 分发——属**聚合分发**，不影响本项目自身的许可 |
| Boost | Boost Software License | 仅服务端 |
| OpenSSL | Apache-2.0 | 仅服务端 |
| nlohmann-json | MIT | 仅服务端 |
