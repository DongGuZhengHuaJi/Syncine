# 内置 yt-dlp 插件

这里的插件随客户端分发、**始终加载**（设置页的「用户插件」开关只控制额外插件目录）。

目录结构与 yt-dlp 的插件约定一致：

```text
plugins/<插件名>/yt_dlp_plugins/extractor/*.py
```

- `douyu/`：斗鱼直播。页面取房间号 → `betard` 房间信息 → `homeH5Enc` 签名 JS →
  `getH5Play` 取 FLV 直链。签名 JS 用随包的 qjs（Linux/macOS）或 Node.js
  （Windows）执行，用户不需要安装 Node。
- 新增内置插件时，把 `<插件名>/yt_dlp_plugins/extractor/*.py` 放在本目录；
  CMake 会把它安装到 `<prefix>/tools/yt-dlp-plugins/`。
- 用户自己安装的插件不要放这里，放到设置页的「额外插件目录」，否则会和内置
  插件一起被加载，且升级客户端时可能被覆盖。
