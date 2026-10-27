#!/usr/bin/env bash
#
# 在 Syncine 服务端所在机器上安装并配置 coturn。
#
# 用法（root）：
#   sudo SYNCINE_PUBLIC_IP=203.0.113.10 bash deploy/coturn/install-coturn.sh
#
# 做的事：
#   1. 安装 coturn（Debian/Ubuntu apt，或 RHEL 系 dnf/yum）
#   2. 生成/复用 static-auth-secret，写 /etc/turnserver.conf
#   3. 把同样的 secret 与 TURN URL 写入 /opt/syncine/env
#   4. 开启 coturn 与系统防火墙端口
#   5. 重启 syncine 服务，让服务端改用自建 coturn
#
# 云厂商安全组仍需手动放行：
#   3478/udp, 3478/tcp, 49160-49300/udp, 49160-49300/tcp
#   （配置 TLS 时再加 5349/tcp）
#
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    echo "请用 root 运行：sudo bash $0" >&2
    exit 1
fi

SYNCINE_HOME=${SYNCINE_HOME:-/opt/syncine}
ENV_FILE="$SYNCINE_HOME/env"
REALM=${SYNCINE_COTURN_REALM:-syncine}
TTL=${SYNCINE_COTURN_TTL:-86400}
TURN_PORT=3478
TLS_PORT=5349
MIN_PORT=49160
MAX_PORT=49300
CONF=/etc/turnserver.conf
DEFAULT_FILE=/etc/default/coturn

if [[ -d /etc/coturn ]]; then
    CONF=/etc/coturn/turnserver.conf
fi
if [[ -d /etc/sysconfig ]]; then
    DEFAULT_FILE=/etc/sysconfig/coturn
fi

# ---- 1. 安装 ----
if command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq coturn openssl curl >/dev/null
elif command -v dnf >/dev/null 2>&1; then
    dnf install -y coturn openssl curl >/dev/null
elif command -v yum >/dev/null 2>&1; then
    yum install -y coturn openssl curl >/dev/null
else
    echo "不支持的发行版：请手动安装 coturn 后重新运行（或用包管理器安装）" >&2
    exit 1
fi

# ---- 2. 网络地址 ----
PRIVATE_IP=$(ip -4 route get 1.1.1.1 2>/dev/null \
    | awk '{for (i = 1; i <= NF; ++i) if ($i == "src") { print $(i+1); exit }}')
if [[ -z "${PRIVATE_IP}" ]]; then
    PRIVATE_IP=$(hostname -I | awk '{print $1}')
fi
if [[ -z "${PRIVATE_IP}" ]]; then
    echo "无法确定本机内网地址" >&2
    exit 1
fi

PUBLIC_IP=${SYNCINE_PUBLIC_IP:-}
if [[ -z "${PUBLIC_IP}" ]]; then
    PUBLIC_IP=$(curl -4 -fsS --max-time 6 https://api.ipify.org 2>/dev/null || true)
fi
if [[ -z "${PUBLIC_IP}" ]]; then
    echo "无法自动获取公网 IP；请用 SYNCINE_PUBLIC_IP=... 指定" >&2
    exit 1
fi

# ---- 3. secret：环境变量 > 已有 env > 新生成 ----
SECRET=${SYNCINE_COTURN_SECRET:-}
if [[ -z "${SECRET}" && -f "${ENV_FILE}" ]]; then
    SECRET=$(grep -m1 '^SYNCINE_COTURN_SECRET=' "${ENV_FILE}" 2>/dev/null \
        | cut -d= -f2- || true)
fi
if [[ -z "${SECRET}" ]]; then
    SECRET=$(openssl rand -hex 32)
fi

# ---- 4. coturn 配置 ----
umask 027
if [[ -f "${CONF}" ]]; then
    cp -a "${CONF}" "${CONF}.bak.$(date +%Y%m%d-%H%M%S)"
fi
cat > "${CONF}" <<EOF
# 由 Syncine deploy/coturn/install-coturn.sh 生成
listening-port=${TURN_PORT}
fingerprint
lt-cred-mech
use-auth-secret
static-auth-secret=${SECRET}
realm=${REALM}
stale-nonce=600
no-cli
no-multicast-peers
no-loopback-peers
total-quota=200
user-quota=64
min-port=${MIN_PORT}
max-port=${MAX_PORT}
# 不让 TURN 变成访问内网的代理
denied-peer-ip=10.0.0.0-10.255.255.255
denied-peer-ip=172.16.0.0-172.31.255.255
denied-peer-ip=192.168.0.0-192.168.255.255
denied-peer-ip=127.0.0.0-127.255.255.255
denied-peer-ip=169.254.0.0-169.254.255.255
EOF

if [[ "${PUBLIC_IP}" != "${PRIVATE_IP}" ]]; then
    cat >> "${CONF}" <<EOF
listening-ip=${PRIVATE_IP}
relay-ip=${PRIVATE_IP}
external-ip=${PUBLIC_IP}/${PRIVATE_IP}
EOF
else
    cat >> "${CONF}" <<EOF
listening-ip=${PRIVATE_IP}
relay-ip=${PRIVATE_IP}
EOF
fi

TURN_URLS="turn:${PUBLIC_IP}:${TURN_PORT}?transport=udp,turn:${PUBLIC_IP}:${TURN_PORT}?transport=tcp"
if [[ -n "${SYNCINE_COTURN_TLS_CERT:-}" && -n "${SYNCINE_COTURN_TLS_KEY:-}" ]]; then
    cat >> "${CONF}" <<EOF
tls-listening-port=${TLS_PORT}
cert=${SYNCINE_COTURN_TLS_CERT}
pkey=${SYNCINE_COTURN_TLS_KEY}
EOF
    TURN_URLS+=",turns:${PUBLIC_IP}:${TLS_PORT}?transport=tcp"
fi

chown root:turnserver "${CONF}" 2>/dev/null || true
chmod 640 "${CONF}"

# ---- 5. 启用 coturn ----
if [[ -f "${DEFAULT_FILE}" ]]; then
    if grep -q '^TURNSERVER_ENABLED=' "${DEFAULT_FILE}"; then
        sed -i 's/^TURNSERVER_ENABLED=.*/TURNSERVER_ENABLED=1/' "${DEFAULT_FILE}"
    else
        echo 'TURNSERVER_ENABLED=1' >> "${DEFAULT_FILE}"
    fi
fi
systemctl daemon-reload
systemctl enable coturn >/dev/null 2>&1 || true
if ! systemctl restart coturn; then
    journalctl -u coturn -n 30 --no-pager || true
    echo "coturn 启动失败，请根据上面的日志检查 ${CONF}" >&2
    exit 1
fi
if ! systemctl is-active --quiet coturn; then
    journalctl -u coturn -n 30 --no-pager || true
    echo "coturn 未处于 active 状态，请检查 ${CONF}" >&2
    exit 1
fi

# ---- 6. 更新服务端 env（保留 Cloudflare 配置和其他行）----
mkdir -p "${SYNCINE_HOME}"
touch "${ENV_FILE}"
chmod 600 "${ENV_FILE}"

# 先写到临时文件再原子替换，避免中途失败把原 env 截断。
TMP_ENV=$(mktemp)
grep -v -E '^SYNCINE_COTURN_(URLS|STUN_URLS|SECRET|TTL)=' "${ENV_FILE}" > "${TMP_ENV}" || true
{
    cat "${TMP_ENV}"
    echo "SYNCINE_COTURN_URLS=${TURN_URLS}"
    echo "SYNCINE_COTURN_STUN_URLS=stun:${PUBLIC_IP}:${TURN_PORT}"
    echo "SYNCINE_COTURN_SECRET=${SECRET}"
    echo "SYNCINE_COTURN_TTL=${TTL}"
} > "${TMP_ENV}.new"
chmod 600 "${TMP_ENV}.new"
chown syncine:syncine "${TMP_ENV}.new" 2>/dev/null || true
mv "${TMP_ENV}.new" "${ENV_FILE}"
rm -f "${TMP_ENV}"

# ---- 7. 系统防火墙（云安全组仍需手动放行）----
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q 'Status: active'; then
    ufw allow "${TURN_PORT}/tcp" >/dev/null || true
    ufw allow "${TURN_PORT}/udp" >/dev/null || true
    ufw allow "${MIN_PORT}:${MAX_PORT}/tcp" >/dev/null || true
    ufw allow "${MIN_PORT}:${MAX_PORT}/udp" >/dev/null || true
    if [[ -n "${SYNCINE_COTURN_TLS_CERT:-}" ]]; then
        ufw allow "${TLS_PORT}/tcp" >/dev/null || true
    fi
fi
if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
    firewall-cmd --permanent --add-port="${TURN_PORT}/tcp" >/dev/null || true
    firewall-cmd --permanent --add-port="${TURN_PORT}/udp" >/dev/null || true
    firewall-cmd --permanent --add-port="${MIN_PORT}-${MAX_PORT}/tcp" >/dev/null || true
    firewall-cmd --permanent --add-port="${MIN_PORT}-${MAX_PORT}/udp" >/dev/null || true
    if [[ -n "${SYNCINE_COTURN_TLS_CERT:-}" ]]; then
        firewall-cmd --permanent --add-port="${TLS_PORT}/tcp" >/dev/null || true
    fi
    firewall-cmd --reload >/dev/null || true
fi

# ---- 8. 重启 Syncine 并检查 ----
if systemctl cat syncine >/dev/null 2>&1; then
    systemctl restart syncine
fi

if ! systemctl is-active --quiet coturn; then
    echo "coturn 启动失败，请查看：journalctl -u coturn -n 50 --no-pager" >&2
    exit 1
fi

echo "coturn 已启动：${PUBLIC_IP}:${TURN_PORT} (UDP/TCP)"
echo "服务器环境已更新：${ENV_FILE}"
echo "云安全组请放行：${TURN_PORT}/udp, ${TURN_PORT}/tcp, ${MIN_PORT}-${MAX_PORT}/udp, ${MIN_PORT}-${MAX_PORT}/tcp"
if [[ -n "${SYNCINE_COTURN_TLS_CERT:-}" ]]; then
    echo "TLS 已启用，另放行：${TLS_PORT}/tcp"
fi
echo "验证命令：systemctl status coturn --no-pager；journalctl -u syncine -n 30 --no-pager | grep Turn"
