# 更新日志

本项目遵循[语义化版本](https://semver.org/lang/zh-CN/)。

## [未发布]

### 共享模式

- **观众端停止推流**：视频与电影声的发送开关改为“共享模式且是房主”才开启，通过
  `encodings[0].active` 真正停止 RTP；观众端同时停止喂帧和 PCM，不再占用上行带宽。
- **建连即停发**：视频轨的初始 `active` 随 `AddTrack` 传入，连接建立后再补写一次，
  避免非共享模式/观众端在建连瞬间先发几帧。
- **黑帧回灌修复**：切换画面时注入的清屏黑帧不再被当成真实解码帧转发给推流端。

- **音画同步**：视频与电影声使用同一个 MediaStream id（`syncine-media`），接收侧 libwebrtc 才会把两条流放进同一 sync_group 做投递对齐。
- **音频积压补偿**：`MovieAudioPlayer::playoutDelayMs()` 用环形缓冲 + QAudioSink 缓冲估算“还要多久响出”；视频按 `到达时间 + 当前延迟` 排期，延迟用时间基非对称 EMA 平滑，队列有界（24 帧 / 64MB）。
- **seek 清缓冲**：观众收到远端 seek 时清空电影声环形缓冲并丢弃 250ms 内来自 NetEq 的旧音频，同时清空视频待显示队列。
- **音频环形缓冲**：`QByteArray::remove(0,n)` 改为固定大小环形缓冲；`OnData` 不再跨线程读 `QAudioSink`。
- **sink 生命周期**：`QVideoSink` 改 `QPointer`，`VideoOutput` 销毁时 `unbindVideoOutput()`；`PeerLink::close()` 对远端音/视频轨 `RemoveSink`。`WebrtcManager::destroy()` 先清延迟回调、再销毁渲染器/播放器。
- **暂停误报**：卸载/换源/播完不再广播 Pause；用户主动暂停仍正常同步。

### 网链

- **手选 format_id**：新条目用 `id:` 前缀记录手选流；裸数字 spec 先判断是否为源内真实分辨率高度，再按 format_id 解释。纯数字 format_id（30080/137）不再被当成档位静默忽略；显式 `id:` 不存在时直接报错，不再回退最高画质。
- **测试骨架**：新增 `Client/tests` 与 CTest，覆盖档位/手选 id/`id:` 前缀/缺失 id/标签显示。

### 日志

- **异步写入**：客户端与服务端改为“调用线程入队 + 单写线程批量落盘”，每 100ms 或 64KB flush 一次；队列有界，满时丢弃 trace/debug 并计数，Warn 及以上保留，Fatal 同步落盘。`SYNCINE_LOG_SYNC=1` 可回退到旧的逐行同步模式。
- **降噪**：直播弹幕的逐包/逐通知日志降到 trace，ICE 候选收集错误降到 debug；文件传输进度和 GetStats 频率不变。
- **WebRTC 日志**：恢复 `SYNCINE_WEBRTC_LOG` 开关，默认只接 warning 及以上。
- **收尾日志**：客户端应用对象在 `Log::shutdown()` 之前析构，WebRTC 停线程等收尾日志不会丢。

## [1.4.0] — 2026-10-28

> 本版客户端与服务端需要一起升级：`video_status` 新增 `itemId/seq`，文件传输的
> DataChannel 名称改为 `syncine-file-ctl` / `syncine-file-data`，新旧客户端不能互传文件。

### TURN

- **自建 coturn 优先**：服务端配置 `SYNCINE_COTURN_URLS` / `SYNCINE_COTURN_SECRET` 后，用 coturn 的 `use-auth-secret` 本地生成 HMAC-SHA1 临时凭据；不再启动 Cloudflare 兑换线程。
- **Cloudflare 回退**：未配置 coturn 时继续使用原来的 Cloudflare TURN 换取流程，配置和密钥保持不变。
- **部署脚本**：新增 `Server/deploy/coturn/install-coturn.sh`，安装并配置 coturn、写入 `/opt/syncine/env`、保留 Cloudflare 配置；Linux 服务端包会带上该脚本。
- 启动日志会明确输出“使用自建 coturn”或“未启用自建 coturn，使用 Cloudflare TURN”；不会记录 secret 或 credential。

### 文件传输

- **控制与数据分离**：控制 JSON 走 `syncine-file-ctl`（reliable + unordered），文件块走 `syncine-file-data`（reliable + ordered）。取消、校验和心跳不再排在大文件块的重传队列后面。
- **应用层 ACK 窗口**：接收端每 128KB 或 100ms 合并回报 `ack`，发送端最多允许 512KB 未确认数据在途，文件块降为 32KB；所有字节确认后才发送 `complete`，避免无序控制通道上 complete 先于最后几块到达接收端。
- **停滞与健康检查**：15 秒没有 ACK / 数据块就转入 Interrupted 并主动 `resume`；控制通道每 5 秒 ping，15 秒没有任何控制消息判定无响应；60 秒仍无法恢复才失败，不再无限卡住。
- **更安全的断点续传**：通道重建后允许按接收端偏移回退重算前缀 SHA-256；仍在存活的旧通道上只从已发送位置继续，避免与 SCTP 中重传的旧块重复写坏文件；重连/恢复时重发丢失的 offer / resume / complete。
- **发送窗口与诊断**：默认发送窗口 512KB（可用 `SYNCINE_TRANSFER_WINDOW_KB` 覆盖，128–16384）；在 100ms RTT 的中继路径上约为 BDP 的 7 倍，减少 relay/SCTP 队列积压和 RTT 尖峰。传输期间每 15 秒输出一条 `WebRTCStats` 诊断日志（选中 candidate 类型、RTT、可用带宽、DataChannel 字节），只在有传输时运行；进度日志从每 2 秒降为每 10 秒。
- **重复发送保护**：同一文件正在传输时直接提示，不再产生多条同名记录。
- **兼容性**：数据通道名由 `syncine-file` 改为 `syncine-file-ctl` / `syncine-file-data`，新旧客户端之间不能互传文件，两端需要一起升级。

### 变更

- **`video_status` 必须携带 `itemId` 与 `seq`**：服务端按当前播放列表条目过滤视频状态、按序号丢弃乱序状态，客户端与服务端需一起升级。
- **移除 HDR 离线转码**：不再生成 `_SDR.mkv`，播放器也不会自动切换到转码产物
- **HDR 下载改为硬约束**：设置关闭时只选 SDR；档位只考虑不超过目标高度的流，没有 SDR 时向更低分辨率退，而不是回退 HDR；下载后用 ffprobe 复核 `color_transfer`，发现 HDR 直接删除并报错
- 条目 spec 的 `:h` 现在也会缀在详情视图手选的 HDR 视频上；手选 HDR 流同样受本地开关和 spec 的双重限制，没有 `:h` 的手选 HDR 流会被拒绝

### 修复

- **切换房间模式误报「视频不匹配」**：`video_status` 增加 `itemId` 与递增的 `seq`，服务端只比较当前条目的状态，并在当前条目变化时清空成员旧的 loaded/hash/时长；迟到或乱序（旧条目、序号更小）的状态直接丢弃。观众端的提示改用独立弹窗，状态恢复一致时自动关闭。
- **房主切换共享模式后观众端残留画面**：`PlaybackController::applyVideoSink()` 在远端/本地画面切换时向 `QVideoSink` 送一帧黑色，清掉上一路留下的最后一帧；`VideoOutput` 不会因为远端渲染器停手就自动变黑。
- **删除播放列表条目时未终止下载**：服务端确认删除后，`PlaylistModel` 发出 `itemRemoved`；`PlaybackSync` 如果发现该条目正是当前解析/下载/合并任务，立即 `cancel()` 并清理进度与失败状态。删除当前条目时同时卸载播放器与弹幕，后续由 `playlist_switched` 加载新的当前条目。
- **下载器 cancel/start 竞态**：`cancel()` 现在同步 kill + waitForFinished + 置空，紧接着的 `start()` 不会再被静默吞掉
- **yt-dlp 卡死**：探测 30 秒、下载 60 秒、合并 3 分钟无输出即主动终止；`FailedToStart` 会清理进程指针，不再永久锁死下载器
- **离开房间不再白起一次 yt-dlp**：修正 clearRoomState 中间状态触发的无意义解析
- **播放列表逐条重试**：每个网链条目新增刷新图标；当前条目失败/卡住时可直接重试，非当前条目在后台下载并写入缓存，不打断播放；失败状态按条目保存，成员表刷新和切条目都不会自动重拉 yt-dlp，切回失败条目仍显示错误

## [1.3.0] — 2026-10-25

### 新增

- **断线重连与会话恢复**
  - 客户端信令断开后保留房间，90 秒内自动重连；新增 10 秒 ping / 15 秒 pong 心跳，静默断网也能尽快发现
  - 服务端 WebSocket 握手 10 秒、空闲 20 秒 + keepalive ping；成员断线后保留 120 秒，不再立即移除或解散房间
  - `resume` 会话恢复：稳定成员 id + token，重连后 clientId 不变，其他成员看不到退出/加入；PeerConnection、DataChannel 和播放状态原样保留
  - 自动重连 90 秒失败后保留凭据，可在加入房间页手动“恢复上次会话”（含房主身份）；重连固定原服务器，不再回退到兜底地址
  - 成员列表显示在线状态：断线成员显示“连接中断，等待重连…”，顶栏只统计在线人数

- **ICE restart**：ICE 失败后用新的 ICE 凭据重新协商，复用原 PeerConnection、轨道和 DataChannel；信令恢复后自动重排中断的重启

- **文件传输断点续传**：DataChannel 断开后进入 Interrupted，60 秒内用 `resume` / `resume_ack` 从 `.part` 已收字节继续；发送端分块重算前缀 SHA-256
- **文件传输拥塞控制**：按 SCTP 发送缓冲 4MB / 1MB 高低水位节流，每拍预算 1MB，避免发送队列无界增长

### 变更

- 服务端房间成员身份改为稳定 clientId（与连接 id 分离），WebRTC 信令、视频状态和播放列表统一使用稳定 id
- 客户端过滤 `member_not_found` / `not_in_room` 这类 WebRTC 信令竞态错误，不再弹窗；离开房间时主动关闭提示框
- **服务端与客户端必须一起升级**：新客户端重连只走 `resume`，旧服务端会返回 `bad_message`

### 修复

- 退出房间时偶发弹出后立即消失的提示框
- 成员断线期间因 ICE 候选持续发往离线成员而产生的大量 `member_not_found` 弹窗

## [1.2.1] — 2026-10-21

### 新增

- **B 站直播弹幕**：网链模式播放直播时，弹幕经 WebSocket 实时接收并显示
  - `room_init` 取真实房间号、`getConf` 取 token 与弹幕服务器，认证后每 30 秒心跳，断线按 3 秒 → 30 秒退避重连
  - 包体按 16 字节头解析；即使认证时声明 `protover=0`，服务器仍会发 zlib 压缩包（ver=2），因此递归解压是硬需求
  - 带 Cookie 时用登录态 `DedeUserID` 认证，匿名连接会被明显限流
  - 直播弹幕没有共同时间轴：不受暂停/seek 影响，轨道占满时轮转复用而不是丢弃

### 修复

- **generic 直链被整条丢弃**：yt-dlp 对 TikTok CDN 等直链给出的 `vcodec` 为 null、`acodec` 缺失，旧逻辑把「编码未知」当成「没有流」，报「没有可下载的流」。现在只有两者都明确为 `"none"` 才按故事板处理；`video_ext` 表明是视频文件时按音视频合一解析，没有 `height` 档位时直接交给播放器
- 直链格式没有分辨率/编码信息时，显示名回退为容器名，不再留空

### 变更

- 客户端构建新增 zlib 依赖（直播弹幕的压缩包体）

### 发布工程

- GitHub Release 增加服务端包：Linux（`Syncine-Server-<版本>-Linux-x86_64.tar.gz`）与 Windows（`Syncine-Server-<版本>-Windows-x64.zip`，静态链接的单个 `Server.exe`）
- 服务端 CI 增加启动冒烟测试：构建后实际启动并确认 8765 端口监听
- 服务端构建显式查找 `nlohmann_json`；Windows 构建定义 `NOMINMAX`，避免 `windows.h` 的 `min`/`max` 宏破坏标准库调用

## [1.2.0] — 2026-10-18

### 新增

- **倍速播放**：同步模式与网链模式支持切换播放速度（共享模式与直播不提供）；倍速随入房状态下发，房间成员保持一致
- **B 站视频弹幕**：网链模式播放 B 站视频时自动拉取并解析弹幕（yt-dlp 的 `danmaku` 字幕轨，缓存为 `danmaku.xml`），按播放进度显示
- **弹幕设置**：显示区域、不透明度、字号、速度档位、字号是否随播放器缩放

### 变更

- 网链模式解析结果保留 yt-dlp 的站点标识与平台内 id（`extractor` / `id`），供弹幕层判断是否为 B 站源

## [1.1.0] — 2026-10-18

### 新增

- **成员间文件传输**：成员列表每行新增邮件入口，可向任意成员发送文件
  - 走已建立的 WebRTC DataChannel（voice 连接），点对点直传，不经过服务器
  - 分块传输（64KB）、进度/速度显示、对方确认后开始、双方均可取消
  - 接收端边收边算 SHA-256，完成后校验并去掉 `.part`；失败原因显示在记录中
  - 发送与接收记录按对端保存，关闭传输窗口不中断任务
- **网链模式下载进度**：播放列表条目显示下载百分比与进度条；画面/声音分离下载时按整体进度折算，不再在换流时倒退

### 变更

- 非共享模式停发电影音轨（`encodings[0].active = false`），不再为无人接收的音轨持续发送静音包；共享模式暂停时仍保留静音流，恢复播放无启动延迟
- 接收端电影音轨诊断日志降噪：静音帧只在前 3 帧提示，周期日志仅在峰值非 0 时输出

### 修复

- 修正 yt-dlp 进度模板的解析错误：`download:` 是模板类型前缀而非输出内容，导致下载进度一直停留在「下载中…」

## [1.0.1] — 2026-10-17

### 平台支持

- **Windows 客户端**：新增 Windows 构建与运行支持
  - `FindWebRTC.cmake` 增加 WIN32 分支（`webrtc.lib`、`WEBRTC_WIN` 平台宏、系统库清单，不走 CREL/lld）
  - MSVC 关闭 RTTI（`/GR-`）；打包时随附 VC++ 运行库；固定 Qt Quick Controls 2 样式
  - 新增 WebRTC 补丁 `0003-win-com-apartment.patch`：修复 Qt 的 STA 与 WebRTC 音频路径 MTA 冲突导致的启动崩溃
  - `Log` / `Toolchain` 增加 Windows 可移植性处理；README 按平台拆分构建与运行说明
- 新增 `scripts/fetch-webrtc.sh`、`scripts/pack-webrtc.sh`，用于下载和打包 WebRTC 预编译产物

### 修复

- **Windows 端「跟随系统默认」落到错误设备**：默认设备不再被翻译成 ADM 索引 0，Windows 上改用 `kDefaultCommunicationDevice`；修复两个 Windows 客户端互相听不到语音的问题
- **单声道电影声被整帧丢弃**：接收端接受单声道并铺成立体声播放；SDP 日志补上 `fmtp` 行，便于排查 `stereo=1` 协商
- **WebRTC 内部日志未落盘**：新增 `WebrtcLog`，默认收集警告及以上；设置 `SYNCINE_WEBRTC_LOG=1` 时包含 INFO

## [1.0.0] — 2026-10-15

首个公开版本。

### 功能

- **三种观看模式**，可随时切换，各自保留独立的播放列表
  - 同步模式：各人持有同一个视频文件，只广播 play / pause / seek；服务端比对内容哈希，版本不一致会提示
  - 共享模式：房主经 WebRTC 推送画面与电影音轨，观众不加载任何媒体
  - 网链模式：房间共享条目（链接 + 画质档位），各端自行解析、下载、合并后播放
- **语音与聊天**：麦克风推流、弹幕式聊天
- **播放列表**：添加 / 删除 / 切集，同步模式下每条带匹配状态（未匹配 / 已匹配 / 不同步，由服务端聚合）
- **网链模式**：yt-dlp 解析链接、画质档位选择、直播支持、下载合并；可从浏览器导入 cookie 以访问需要登录的站点
- **HDR 处理**：检测 HDR 片源并可选转码为 SDR（避免过曝）
- **音频设备选择**：麦克风与听筒可在运行时切换
- **TURN 支持**：服务端用 Cloudflare 密钥现场兑换临时凭据随入房消息下发，密钥只留在服务端
- 可调日志级别，两端日志格式一致

### 发布工程

- `LICENSE`（MIT）、`README.md`、`CHANGELOG.md`、`.gitattributes`
- CMake：版本号单一来源（`project(... VERSION)`）、`install()` 规则、CPack（TGZ + ZIP）、Qt 部署脚本、release preset
- WebRTC 的路径、补丁与 GN 参数全部入库（`cmake/FindWebRTC.cmake`、`third_party/webrtc/`），不再写死开发者机器路径
- `scripts/`：`build-webrtc.sh`（复现 WebRTC 构建）、`fetch-tools.sh`（下载 ffmpeg / yt-dlp 并校验哈希）
- GitHub Actions 工作流骨架（Linux 可用；Windows job 待 WebRTC 的 Windows 产物就绪后启用）

### 修复

发布前做了一轮完整的代码审查（含 AddressSanitizer 实测），修掉以下问题：

**服务端**

- **一条消息即可打挂服务端**：`message.value()` 在字段类型不符时抛出的 nlohmann 异常没有被捕获，会穿透工作线程触发 `std::terminate`。发 `[1,2,3]` 或 `{"type":"chat","text":42}` 就能复现（后者连房间都不用进）。现在整个消息处理有异常兜底
- **写缓冲区的 use-after-free**：`async_write` 不拷贝载荷，而承载数据的是局部 `std::string`，函数返回即析构——对端读得慢时内核会去读已释放的内存。ASan 复现为 heap-use-after-free；不开 ASan 时表现为把堆上残留数据写给对端（内存信息泄漏）
- **伪造 `itemId` 导致内存无上限增长**：`playlist_status` 不校验条目是否存在就直接建表项，实测 3000 条消息让常驻内存 +13 MB 且永不释放。现在必须命中列表中的真实条目
- **配额**：房间数、单房间成员数、播放列表长度、单条消息大小都加了上限。此前单连接循环 `playlist_add` 即可让列表无限增长，而每次添加都要全量序列化整个列表并群发（成本 O(n²)），且全程持有全局房间锁
- **端口被占时不再自旋**：绑定失败后进程不退出、继续在没绑上的套接字上 accept，实测单核 100% 而 systemd 看不出异常。现在直接以非零码退出
- **TURN 凭据兑换会永久挂死**：`beast::tcp_stream` 的 `expires_after()` **只对异步操作生效**，而兑换全程用的是同步调用，那些 15 秒超时形同虚设。部署到 VPS 后一次 TLS 层停顿就让 TURN 线程永久挂在 `tcp_recvmsg` 上——后果不止是这次拿不到凭据：重试计时器只在兑换函数返回后才更新，所以**重试循环根本进不去**，服务端会一直静默退化成纯 STUN。同步调用要真正的超时只能从外部掐断套接字（`shutdown(2)`），现在由看门狗线程兜底
- 退出时 `Log::shutdown()` 早于对象析构，收尾日志进不了文件；客户端字符串直接进日志（可用换行伪造日志行、超长标题冲垮轮转）

**客户端**

- **栈溢出崩溃（可被房间内他人触发）**：源里没有带 `height` 的视频流时（纯音频源、部分 HLS、仅 storyboard），档位挑选会用完全相同的参数无限递归。spec 是条目级、随房间下发的，一个人加进这样一条，全房间切到它时一起崩。现已改为返回「没有可下载的流」并给出提示
- **yt-dlp 参数注入**：房间成员的 URL 是作为裸 argv 元素传给 yt-dlp 的，任何以 `-` 开头的元素会被 optparse 当成**选项**执行（实测 `--version` 会打印版本号并正常退出）。现在 URL 前插 `--` 终止选项解析，并加了 http/https scheme 白名单
- **下载的影片永不清理**：退出时的清理逻辑拿不到房间号——`leaveRoom()` 先清空 `m_roomId` 再发信号，于是删除目标是 `<临时目录>/unknown` 这个不存在的路径。现在删整个客户端临时目录
- 兜底服务器地址仍是开发机的 `ws://localhost:8765`，未手动填地址的用户永远连不上
- QML 引擎晚于日志系统析构，退出阶段的告警既进不了日志文件也看不见

### 已知限制

见 README「已知限制」一节：信令未加密、房间号可枚举、播放控制无角色校验、音画同步未实现等。
