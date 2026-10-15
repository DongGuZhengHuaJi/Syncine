# 外部工具

网链模式(URL 播放)需要的两个**命令行**工具。

> 别和 Qt 那套 FFmpeg 搞混:Qt Multimedia 用的是随 Qt 分发的**动态库**
> (`libavcodec.so.61` 等,在 Qt 的 lib 目录里),从不调用可执行文件;
> 这里放的是**独立的程序**,只有 yt-dlp 会调用它。两者互不感知。

| 文件 | 版本 | 来源 | 许可 |
|---|---|---|---|
| `yt-dlp/yt-dlp_linux` | 2026.08.19 | `github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux` | 源码 Unlicense(公有领域);**打好的可执行文件因含 PyInstaller/GPLv3+ 组件,整体按 GPLv3+ 分发** |
| `ffmpeg/ffmpeg` | N-127271(2026-10-10) | BtbN/FFmpeg-Builds 的 `ffmpeg-master-latest-linux64-lgpl.tar.xz` | LGPL v3(见 `ffmpeg/LICENSE.txt`) |
| `ffmpeg/ffprobe` | 同上 | 同上 | 同上 |

**为什么选 lgpl 版 ffmpeg**:合并音视频只需要转封装(`-c copy`,不重新编码),
用不到 x264/x265 这些 GPL-only 编码器 —— LGPL 版完全够用,分发义务最轻。

**体积**:`ffmpeg` 与 `ffprobe` 各约 139MB,`yt-dlp` 约 40MB。`ffplay` 用不上,没放。
要瘦身可以删掉 `ffprobe`(省 139MB),代价是 yt-dlp 少一些修复能力
(比如 m3u8 下载后修时间戳),它会在日志里给警告。

## 怎么更新

yt-dlp **更新很频繁**——网站一改版旧版就失效。更新就是替换文件:

```bash
curl -L -o tools/yt-dlp/yt-dlp_linux \
  https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux
chmod +x tools/yt-dlp/yt-dlp_linux
```

ffmpeg 很少需要更新;要更新就重下最新的 `linux64-lgpl.tar.xz`,
解出 `bin/ffmpeg`、`bin/ffprobe` 覆盖过来。

## 应用怎么找到它们(`src/core/Toolchain.cpp`)

按顺序找:

1. 可执行文件旁边的 `tools/<名字>/`(打包后的布局)
2. **构建目录的上一级**的 `tools/<名字>/`(开发时就是本目录)
3. `~/.local/share/Syncine/bin/`(留给以后"应用自己更新 yt-dlp"用)
4. 系统 `PATH`(能找到但版本可能旧,日志里会打出来是哪个)

找到之后,应用会把路径和版本写进日志(`[Toolchain]` 开头);
调 yt-dlp 时**显式传 `--ffmpeg-location`**,不让它去 PATH 里挑到系统那份旧 ffmpeg。

## 跨平台

二进制是平台相关的,现在只放了 **linux-x86_64**。以后做别的平台要各放各的:
`yt-dlp.exe` / `yt-dlp_macos`,ffmpeg 用 BtbN 的 `win64`(macOS 要另找来源)。

## 应用怎么用它们(网链模式)

1. **解析**:`yt-dlp -J --no-playlist <链接>` 拿标题/时长/格式列表
2. **两条路**(详解见 `src/core/UrlDownloader.h` 顶部):
   - **直链流播** —— 站点提供单文件格式(画面和声音在同一个流里)时,把直链直接交给
     QMediaPlayer:秒开、能拖进度、不落盘。
     ⚠ 需要 `Referer`/`Cookie` 的直链(比如 B 站)用不了 —— 播放器加不了自定义头
     (**导入了 cookie 也一样**:cookie 只作用于 yt-dlp,也就是"解析"和"下载"那两条路),
     这类自动降级到下面这条。
   - **下载合并** —— 高画质基本是 DASH(画面/声音分成两条流),用
     `-f "bv*+ba/b" --merge-output-format mkv` 下完再合并。**这一步用的就是这里的 ffmpeg。**
3. **临时文件**:下载到系统临时目录的 `syncine/<房间号>/<条目id>/`,
   切走或离开房间就删。**不做持久缓存** —— 同一部片子再看会重新下。
