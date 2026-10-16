#!/usr/bin/env bash
#
# 下载 WebRTC 预编译产物并解压，供构建客户端使用。
#
# 开发与 CI 用。维护者从源码重新构建 WebRTC 后用
# scripts/pack-webrtc.sh 打包上传（见 third_party/webrtc/README.md 第 5 节），
# 使用方用本脚本取用，无需自己构建。
#
# 用法：
#   ./scripts/fetch-webrtc.sh <下载地址> [目标目录]
#   WEBRTC_ASSET_URL=<下载地址> ./scripts/fetch-webrtc.sh [目标目录]
#
# 环境变量：
#   WEBRTC_ASSET_URL  下载地址；也可作为第一个参数给出
#   WEBRTC_SHA256     期望的 SHA-256；给出则校验，不给则跳过
#                     注意：跳过校验意味着无法确认二进制是否被替换过
#
# 完成后打印可直接使用的 -DWEBRTC_ROOT 值。

set -euo pipefail

die() { echo "错误：$*" >&2; exit 1; }

# 参数按内容区分：http(s) 开头的当下载地址，其余当目标目录。
# 这样 URL 也可以只通过环境变量 WEBRTC_ASSET_URL 提供。
URL="${WEBRTC_ASSET_URL:-}"
DEST=""
for arg in "$@"; do
    case "$arg" in
        http://*|https://*) URL="$arg" ;;
        *)                  DEST="$arg" ;;
    esac
done
DEST="${DEST:-$PWD}"

[ -n "$URL" ] || die "未指定下载地址。用法：$0 <下载地址> [目标目录]
或设置环境变量 WEBRTC_ASSET_URL。"

for cmd in curl tar; do
    command -v "$cmd" >/dev/null 2>&1 || die "找不到 $cmd"
done
# 压缩包是 zstd 格式，tar 需要能调起 zstd
command -v zstd >/dev/null 2>&1 || die "找不到 zstd（Ubuntu 上：apt install zstd）"

ARCHIVE="$(basename "${URL%%\?*}")"
[ -n "$ARCHIVE" ] || die "无法从地址推断文件名：${URL}"

mkdir -p "$DEST"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "下载地址 : ${URL}"
echo "目标目录 : ${DEST}"
echo "正在进行……"

curl -fL --retry 3 --connect-timeout 20 -o "${TMP}/${ARCHIVE}" "$URL" \
    || die "下载失败：${URL}"

# ---- 校验 ------------------------------------------------------------
if [ -n "${WEBRTC_SHA256:-}" ]; then
    GOT="$(sha256sum "${TMP}/${ARCHIVE}" | cut -d' ' -f1)"
    if [ "$GOT" != "$WEBRTC_SHA256" ]; then
        cat >&2 <<EOF
错误：产物哈希与期望不符，已终止。

  期望：${WEBRTC_SHA256}
  实际：${GOT}

产物可能已被替换。确认来源可信后再更新 WEBRTC_SHA256。
EOF
        exit 1
    fi
    echo "哈希校验 : 通过"
else
    echo "哈希校验 : 已跳过（未提供 WEBRTC_SHA256）"
    echo "           解压前会打印实际哈希，供你自行核对"
    echo "           实际 SHA-256: $(sha256sum "${TMP}/${ARCHIVE}" | cut -d' ' -f1)"
fi

# ---- 解压 ------------------------------------------------------------
# 压缩包内已含顶层目录（webrtc-linux-<架构>-<revision>），直接解到 DEST 即可
tar --zstd -xf "${TMP}/${ARCHIVE}" -C "$DEST" || die "解压失败"

# 顶层目录名 = 归档名去掉压缩后缀
ROOT_NAME="${ARCHIVE%.tar.zst}"
ROOT_NAME="${ROOT_NAME%.tar.xz}"
ROOT_NAME="${ROOT_NAME%.tgz}"
ROOT="${DEST}/${ROOT_NAME}"
[ -d "$ROOT" ] || die "解压后未找到预期目录：${ROOT}"

# ---- 校验内容完整性 --------------------------------------------------
MISSING=0
for item in \
    "out/Default/obj/libwebrtc.a" \
    "out/Default/gen" \
    "third_party/abseil-cpp" \
    "third_party/libyuv/include" \
    "third_party/perfetto/include" \
    "third_party/llvm-build/Release+Asserts/bin/ld.lld"
do
    if [ ! -e "${ROOT}/${item}" ]; then
        echo "  缺失：${item}" >&2
        MISSING=$((MISSING + 1))
    fi
done
[ "$MISSING" -eq 0 ] || die "产物内容不完整（缺 ${MISSING} 项），已解压的目录请自行删除"

echo
echo "完成。配置客户端时指定："
echo
echo "  export WEBRTC_ROOT=${ROOT}"
echo
echo "或作为 CMake 参数："
echo
echo "  cmake -DWEBRTC_ROOT=${ROOT} ..."
