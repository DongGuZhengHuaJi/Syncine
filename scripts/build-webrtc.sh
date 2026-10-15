#!/usr/bin/env bash
#
# 从源码构建 Syncine 用的 libwebrtc.a。
#
# 维护者用。终端用户和 CI 不该跑这个 —— 应当下载预编译产物（见
# third_party/webrtc/README.md 第 5 节）。
#
# 首次同步要拉几 GB，编译要几小时。请确保磁盘有 100 GB 以上空间。
#
# 用法：
#   WEBRTC_CHECKOUT=~/webrtc-checkout ./scripts/build-webrtc.sh
#
# 全流程：同步到钉死的 revision → 打补丁 → GN 配置 → ninja 编译。

set -euo pipefail

# ---- 与 third_party/webrtc/README.md 保持一致，改一处要改两处 ----
WEBRTC_REV="bf31ff6fff0088bbbce706720e6f22be6f5294fa"
CHECKOUT="${WEBRTC_CHECKOUT:-$HOME/webrtc-checkout}"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATCH_DIR="${REPO_ROOT}/third_party/webrtc/patches"
OUT_DIR="Default"

# GN 参数必须逐字照抄 third_party/webrtc/README.md 第 3 节。
# is_debug=false 决定 -DNDEBUG，客户端的宏必须与之对齐（否则 sizeof 不符，
# 启动即段错误）。详见 cmake/FindWebRTC.cmake 里的长注释。
GN_ARGS='
use_custom_libcxx = false
use_sysroot = false
treat_warnings_as_errors = false
rtc_include_tests = false
is_debug = false
'

die() { echo "错误：$*" >&2; exit 1; }

for cmd in gclient gn ninja git; do
    command -v "$cmd" >/dev/null 2>&1 \
        || die "找不到 $cmd，depot_tools 装了吗？(https://commondatastorage.googleapis.com/chromium/src/tools/depot_tools.git)"
done

[ -d "${CHECKOUT}" ] || die "目录不存在：${CHECKOUT}（先把 depot_tools 的 gclient 初始化起来）"

# ---- 1. 同步到钉死的 revision ----------------------------------------
echo "==> 同步 WebRTC 到 ${WEBRTC_REV}"
cd "${CHECKOUT}"
gclient sync --revision "src@${WEBRTC_REV}" --no-history

# ---- 2. 打补丁 --------------------------------------------------------
echo "==> 应用补丁"
cd "${CHECKOUT}/src"
for p in "${PATCH_DIR}"/*.patch; do
    [ -e "$p" ] || die "补丁目录是空的：${PATCH_DIR}"
    if git apply --check "$p" 2>/dev/null; then
        git apply "$p"
        echo "    已应用 $(basename "$p")"
    elif git apply --check --reverse "$p" 2>/dev/null; then
        echo "    跳过 $(basename "$p")（已经打过了）"
    else
        # 最可能的原因：上游改了这块代码。这正是不追 main 的理由。
        die "补丁打不上：$(basename "$p")
    说明当前 revision 与补丁不匹配。确认 WEBRTC_REV 是 ${WEBRTC_REV}。"
    fi
done

# ---- 3. GN 配置 -------------------------------------------------------
echo "==> 配置 GN（out/${OUT_DIR}）"
gn gen "out/${OUT_DIR}" --args="${GN_ARGS}"

# ---- 4. 编译 ----------------------------------------------------------
echo "==> 编译（几小时，去泡杯茶）"
ninja -C "out/${OUT_DIR}"

# ---- 5. 结果 ----------------------------------------------------------
LIB="${CHECKOUT}/src/out/${OUT_DIR}/obj/libwebrtc.a"
LLD="${CHECKOUT}/src/third_party/llvm-build/Release+Asserts/bin/ld.lld"

echo
echo "完成。"
echo "  静态库  : ${LIB}"
echo "  链接器  : ${LLD}"
echo
echo "两个都必需 —— 链接器要能认 CREL 重定位，用系统 lld 会静默产出坏二进制。"
echo "构建客户端："
echo "  cmake -DWEBRTC_ROOT=${CHECKOUT}/src ..."
