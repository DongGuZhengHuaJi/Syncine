#!/usr/bin/env bash
#
# 把本地已编好的 WebRTC 打包成可分发的预编译产物。
#
# 产物用于分发给他人或供 CI 使用，使其无需从源码重新构建（见
# third_party/webrtc/README.md 第 5 节）。客户端通过
# cmake -DWEBRTC_ROOT=<解压目录> 引用它。
#
# 用法：
#   ./scripts/pack-webrtc.sh [输出目录]
#
# 环境变量：
#   WEBRTC_CHECKOUT  检出目录，默认 $HOME/webrtc-checkout
#
# 打包策略：只收编译客户端所需的文件，不收源码。
#   src 树约 32 GB，其中绝大部分是 .cc 源文件、测试数据和构建中间产物；
#   客户端只需要头文件、生成的头文件、静态库和链接器，合计约 660 MB。

set -euo pipefail

CHECKOUT="${WEBRTC_CHECKOUT:-$HOME/webrtc-checkout}"
SRC="${CHECKOUT}/src"
OUT_DIR="${1:-$PWD}"
ZSTD_LEVEL="${ZSTD_LEVEL:-15}"

LIB="${SRC}/out/Default/obj/libwebrtc.a"
GEN="${SRC}/out/Default/gen"
LLD_DIR="${SRC}/third_party/llvm-build/Release+Asserts/bin"

die() { echo "错误：$*" >&2; exit 1; }

# ---- 前置检查 --------------------------------------------------------
[ -d "$SRC" ] || die "找不到源码目录：${SRC}（用 WEBRTC_CHECKOUT 指定检出路径）"
[ -f "$LIB" ] || die "找不到静态库：${LIB}（先跑 scripts/build-webrtc.sh）"
[ -d "$GEN" ] || die "找不到生成的头文件目录：${GEN}"
[ -x "${LLD_DIR}/lld" ] || die "找不到链接器：${LLD_DIR}/lld"

for cmd in tar zstd git; do
    command -v "$cmd" >/dev/null 2>&1 || die "找不到 $cmd"
done

REV="$(git -C "$SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
REV_SHORT="${REV:0:8}"
ARCH="$(uname -m)"
case "$ARCH" in
    x86_64) ARCH="x64" ;;
    aarch64) ARCH="arm64" ;;
esac
NAME="webrtc-linux-${ARCH}-${REV_SHORT}"
OUT="${OUT_DIR}/${NAME}.tar.zst"

echo "检出目录 : ${CHECKOUT}"
echo "revision : ${REV}"
echo "产物     : ${OUT}"
echo

# ---- 收集文件清单 ----------------------------------------------------
# 路径写成相对 CHECKOUT 的形式（src/...），后面用 --transform 换成产物名。
LIST="$(mktemp)"
trap 'rm -f "$LIST"' EXIT

cd "$CHECKOUT"

# 1) 头文件。除 .h/.hpp 外还要收 .hh/.hxx/.inc/.inl/.ipp/.tcc/.def ——
#    WebRTC 树里这几种都真实存在，且都可能被 #include。
#    宁可多收几十 MB，也不要漏掉一个被间接引用的头文件：那类错误表现为
#    编译时 "fatal error: xxx 不存在"，且指向 WebRTC 内部文件，很难定位到
#    是打包漏了。
find src -path src/.git -prune -o -path src/out -prune -o -type f \
     \( -name '*.h' -o -name '*.hpp' -o -name '*.hh' -o -name '*.hxx' \
        -o -name '*.inc' -o -name '*.inl' -o -name '*.ipp' \
        -o -name '*.tcc' -o -name '*.def' \) -print >> "$LIST"

# 2) 生成的头文件目录（protobuf 之类），全部收下
find src/out/Default/gen -type f -print >> "$LIST"

# 3) 静态库
echo "src/out/Default/obj/libwebrtc.a" >> "$LIST"

# 4) 链接器。只要 lld 与 ld.lld 软链两个文件：整个 bin/ 目录 310 MB，
#    里面绝大部分是 clang、llvm-objdump 等本项目用不到的 LLVM 工具。
echo "src/third_party/llvm-build/Release+Asserts/bin/lld" >> "$LIST"
echo "src/third_party/llvm-build/Release+Asserts/bin/ld.lld" >> "$LIST"

COUNT="$(wc -l < "$LIST")"
[ "$COUNT" -gt 1000 ] || die "文件清单只有 ${COUNT} 项，明显不对，已中止"

echo "文件数   : ${COUNT}"
echo "正在压缩（zstd -${ZSTD_LEVEL}，多线程）……"

# ---- 打包 ------------------------------------------------------------
# --transform 把 src/ 前缀换成产物名，使解压后即可直接作为 WEBRTC_ROOT 使用。
tar -I "zstd -T0 -${ZSTD_LEVEL}" -cf "$OUT" \
    --transform "s,^src/,${NAME}/," \
    --no-recursion \
    -T "$LIST"

# ---- 结果 ------------------------------------------------------------
echo
echo "完成。"
ls -lh "$OUT" | awk '{print "  大小   : "$5}'
echo "  文件数 : $(tar -I zstd -tf "$OUT" | wc -l)"
echo "  SHA256 : $(sha256sum "$OUT" | cut -d' ' -f1)"
echo
echo "校验方式（解压后目录名即 ${NAME}）："
echo "  tar --zstd -xf $(basename "$OUT") -C <目标目录>"
echo "  cmake -DWEBRTC_ROOT=<目标目录>/${NAME} ..."
