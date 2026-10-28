# coturn 部署

这是 Syncine 服务端自带的 coturn 安装脚本。它把 coturn 作为服务端部署的一部分：

- 安装 coturn；
- 生成/复用 `static-auth-secret`；
- 写入 `/etc/turnserver.conf`；
- 把同一份 secret 和 TURN URL 写入 `/opt/syncine/env`；
- 重启 `syncine`，服务端随即改用自建 coturn；
- 保留 `/opt/syncine/env` 里原有的 Cloudflare 配置，删除 `SYNCINE_COTURN_*` 即恢复 Cloudflare。

## 用法

在服务器上以 root 运行：

```bash
sudo SYNCINE_PUBLIC_IP=203.0.113.10 \
     bash deploy/coturn/install-coturn.sh
```

`SYNCINE_PUBLIC_IP` 是客户端能访问到的公网 IP。脚本也会尝试用 `api.ipify.org`
自动探测；VPS 在 NAT 后时它无法从本机路由表得到公网 IP，建议显式传入。

可选环境变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `SYNCINE_PUBLIC_IP` | 自动探测 | TURN 公网地址 |
| `SYNCINE_COTURN_SECRET` | 复用 env / 随机生成 | 必须与 coturn 的 `static-auth-secret` 一致 |
| `SYNCINE_COTURN_TTL` | `86400` | 临时凭据有效期（秒），范围 600–86400 |
| `SYNCINE_COTURN_REALM` | `syncine` | coturn realm |
| `SYNCINE_COTURN_TLS_CERT` / `_KEY` | 空 | 证书和私钥路径；提供后额外开启 5349/tls |

脚本会生成以下 `/opt/syncine/env`：

```text
SYNCINE_COTURN_URLS=turn:<公网IP>:3478?transport=udp,turn:<公网IP>:3478?transport=tcp
SYNCINE_COTURN_STUN_URLS=stun:<公网IP>:3478
SYNCINE_COTURN_SECRET=<随机 secret>
SYNCINE_COTURN_TTL=86400
```

## 端口

必须在云厂商安全组放行：

- `3478/udp`
- `3478/tcp`
- `49160-49300/udp`
- `49160-49300/tcp`
- 配置 TLS 时再加 `5349/tcp`

系统防火墙由脚本尝试用 ufw/firewalld 放行；云安全组无法从实例内修改，需要手动操作。

## 验证

```bash
systemctl status coturn --no-pager
ss -lunpt | grep 3478
journalctl -u syncine -n 30 --no-pager | grep Turn
```

服务端启动日志应出现：

```text
使用自建 coturn：2 个 TURN URL / 1 个 STUN URL，TTL 86400s
```

客户端日志里的 `WebRTCStats` 应看到 `local=/remote=` 为 relay 时 RTT 明显低于
Cloudflare 路径；如果客户端仍然显示 Cloudflare 的 relay，检查客户端是否使用了
旧的服务端消息缓存（重新进房）或服务端是否重启成功。

## TLS（可选）

`turns:` 需要服务器有可被客户端验证的证书（通常是域名 + Let's Encrypt）。
没有证书时只使用 `turn:...:3478?transport=udp/tcp`，这已经能覆盖大多数
NAT/防火墙场景。

## 回退 Cloudflare

删除 `/opt/syncine/env` 里的 `SYNCINE_COTURN_*` 四行，重启 `syncine`；
服务端会回到原来的 Cloudflare TURN 兑换流程。Cloudflare 的密钥不需要改动。
