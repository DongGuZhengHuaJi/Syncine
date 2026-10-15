# Syncine

基于 WebRTC 的**同步观影**软件。开一个房间，和朋友一起看同一部片子——进度、暂停、快进都是同步的；想看同一个画面又不想上传片源时，房主的画面和电影音轨会经 WebRTC 直接推给其他人。

- **客户端**：Qt 6.10（QML）+ 原生 libwebrtc
- **服务端**：C++ / Boost.Asio + Beast，一个 WebSocket 房间服务器
- **跨平台**：目前只支持 Linux

---

## 三种观看模式

房间可以随时切换模式，**每种模式各存一份播放列表**，切回时仍在。

| 模式 | 谁有片源 | 怎么同步 |
|---|---|---|
| **同步模式** `local` | 每人各自持有同一个视频文件 | 只广播 play / pause / seek，各端播各自的文件。服务端比对内容哈希，有人在看别的版本会提示 |
| **共享模式** `share` | 只有房主有 | 房主的画面与电影音轨经 WebRTC 推给观众（观众不加载任何媒体）；观众不参与排片 |
| **网链模式** `url` | 各自从同一个链接下载 | 房间共享条目（链接 + 画质档位），各端各自解析、下载、合并再播放，房主广播位置供其他人纠偏 |

此外还有语音（麦克风）和聊天、弹幕。

---

## 快速开始

### 直接下载运行（Linux x86-64）

从 [Releases](https://github.com/DongGuZhengHuaJi/Syncine/releases) 下载 `Syncine-1.0.0-Linux.tar.gz`，解压后直接跑：

```bash
tar xzf Syncine-1.0.0-Linux.tar.gz
cd Syncine-1.0.0-Linux
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
> 注意：Linux 包里那个 `.zip` 不是 Windows 程序，Windows 构建见下文「跨平台」。

### 连服务器

客户端默认连 `ws://8.148.12.233:8765`。也可以在「设置」里改成自己的服务器地址（`ws://` 或 `wss://`）。

---

## 从源码构建

### 客户端

**依赖**

| 依赖 | 版本 | 说明 |
|---|---|---|
| Qt | ≥ 6.8（本项目用 6.10.2） | 电影音轨用到了 `QAudioBufferOutput`，6.8 才引入 |
| 编译器 | clang | libwebrtc.a 是 Chromium 的 clang 编的 |
| 链接器 | lld ≥ 19.1 | libwebrtc.a 用实验性的 **CREL 重定位**，见下 |
| WebRTC | 见 [`third_party/webrtc/README.md`](third_party/webrtc/README.md) | 最大的门槛 |
| ffmpeg / yt-dlp | 见 [`Client/tools/README.md`](Client/tools/README.md) | 网链模式需要，由脚本下载 |

```bash
# 1. 随包工具（约 320 MB，不入仓库）
./scripts/fetch-tools.sh

# 2. WebRTC —— 二选一
#    a) 自己编（几小时）：见 third_party/webrtc/README.md
#    b) 下载预编译产物（Release 附件），解压到任意目录
export WEBRTC_ROOT=/path/to/webrtc-checkout/src

# 3. 构建
cmake --preset release          # 在 Client/ 目录下执行
cmake --build --preset release

# 4. 打包
cd build-release && cpack       # 出 Syncine-1.0.0-Linux.tar.gz 与 .zip
```

Qt 与 WebRTC 的位置都从**环境变量/preset** 读取，不写死在仓库里。Qt 不在默认位置时用 `-DCMAKE_PREFIX_PATH=/path/to/Qt/6.10.2/gcc_64` 覆盖。

**关于 CREL 重定位**：`libwebrtc.a` 的对象文件用的是 Chromium 的实验格式（节名 `.crel.text`）。系统自带的 lld < 19.1 不认识它——**不报错**，只是把那些节当未知节静默忽略，结果所有重定位都没被施加，产出启动即段错误、且从源码完全看不出问题的可执行文件。构建脚本会把随 WebRTC 一起编出来的那份 lld 指给链接器。

### 服务端

```bash
cd Server
cmake -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel
./build-release/Server          # 监听 0.0.0.0:8765
```

依赖 Boost（Beast 是 header-only，只要头文件）与 OpenSSL。地址和端口在 `Server/src/tools/Config.h`。

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
- 需要放行 **8765/tcp**（云服务商的安全组也要放行，不只是系统防火墙）。
- 服务端**不自带 TLS**。公网部署建议前面挂一层反向代理（nginx / caddy）做 `wss://` —— 否则房间口令、聊天内容和下发的 TURN 临时凭据都是明文。

---

## 跨平台

| 平台 | 状态 |
|---|---|
| Linux x86-64 | ✅ 1.0 支持 |
| Windows | ❌ 尚未构建。缺的不是 CMake 分支，而是 **Windows 版的 WebRTC**：Chromium 的构建系统在 Windows 上强制要求 MSVC，无法从 Linux 交叉编译，必须在 Windows 机器上重跑一遍 `scripts/build-webrtc.sh` 的流程（数小时）。`cmake/FindWebRTC.cmake` 遇到非 Linux 会直接 `FATAL_ERROR`，就是为了避免悄悄产出坏二进制。CI 里已备好 Windows job 的骨架（`.github/workflows/release.yml`，当前注释着） |
| macOS | ❌ 未验证 |

---

## 已知限制（1.0）

- **信令未加密**：服务端是明文 `ws://`。房间口令、聊天、TURN 临时凭据可被同路径的中间人读到。媒体面走 WebRTC 自带加密（DTLS-SRTP），不受影响。有域名后在客户端改 `wss://` 并前置反代即可。
- **房间号只有 6 位**且入房失败没有节流，公网服务端存在枚举与口令爆破的可能。房间设个口令能挡掉一部分。
- **房间成员都能控制播放**（play / pause / seek 全员可发），这是「和朋友一起看」的设计，但在任何人都能猜号入房的公网服务器上也是打扰手段。
- 音画同步（lip sync）未实现；WebRTC 重协商逻辑不完整。
- 未做时钟漂移校准：位置同步是「动作广播 + 房主定期广播位置」，长时间播放后各端可能有秒级偏差。

---

## 许可证

本项目采用 [MIT](LICENSE)。

随包分发与链接的第三方组件：

| 组件 | 许可证 | 备注 |
|---|---|---|
| Qt 6 | LGPLv3 | **动态链接**分发，满足 LGPL 对「允许用户替换 Qt」的要求 |
| WebRTC | BSD-3-Clause | 本地补丁见 `third_party/webrtc/patches/` |
| ffmpeg | LGPLv3 | 用的是 BtbN 的 **lgpl** 构建（只做转封装，不需要 GPL 编码器）。`tools/ffmpeg/LICENSE.txt` |
| yt-dlp | Unlicense（源码） | 打好的可执行文件含 PyInstaller/GPLv3+ 组件，整份按 GPLv3+ 分发——属**聚合分发**，不影响本项目自身的许可 |
| Boost | Boost Software License | 仅服务端 |
| OpenSSL | Apache-2.0 | 仅服务端 |

发版流程见 [`RELEASING.md`](RELEASING.md)。
