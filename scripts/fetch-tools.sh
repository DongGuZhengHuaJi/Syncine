#!/usr/bin/env bash
#
# 下载随包分发的命令行工具（ffmpeg / ffprobe、yt-dlp）到 Client/tools/。
#
# 这些二进制加起来约 320 MB，不入仓库，用这个脚本获取。
#
# 用法：
#   ./scripts/fetch-tools.sh            # 下载当前平台
#   ./scripts/fetch-tools.sh --force    # 已存在也重新下载
#
# 设计取舍：
#   yt-dlp 按**版本号**钉死（URL 里带版本），哈希严格校验。
#   ffmpeg 用 BtbN 的 master-latest（该 URL 始终指向最新构建），版本无法
#   钉死 —— 下面的哈希只是「最近一次记录过的构建」，上游重新构建后必然
#   对不上。此时只告警并继续安装（CI 跟随上游最新），不中断打包。
#   要严格复现，请把 FFMPEG_URL 换成一个带日期标签的具体资源，并把
#   verify 的 strict 参数改回 1。

set -euo pipefail

# ---- 版本与哈希（本仓库发布 1.0.0 时验证过的）-------------------------
YTDLP_VERSION="2026.08.19"
YTDLP_SHA256_linux_x86_64="58162f9bfdc27458ea47bfcb311cf47028f17d8154a8bf7d689861d46399230a"

# ffmpeg 来自 BtbN/FFmpeg-Builds 的 lgpl 版（只做转封装，不需要 GPL 编码器，
# 分发义务最轻）。下列哈希记录的是最近一次人工核对过的构建。
FFMPEG_BUILD="N-127271-gba987fe24d-20261010"
FFMPEG_SHA256_linux_x86_64_ffmpeg="3b22c09db3600f234ef317ca61609c86d605359ca4164b8801f7e9e5bdb435ae"
FFMPEG_SHA256_linux_x86_64_ffprobe="97803e1c1034443c6e696a72be65f1a17c6d8b2335a0eb33cb28204d2a9e8074"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${REPO_ROOT}/Client/tools"
FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

die() { echo "错误：$*" >&2; exit 1; }
warn() { echo "警告：$*" >&2; }

# 校验哈希。第二个参数为空表示「没有记录可用的哈希」。
# strict=1（默认）：不符即失败；strict=0：只告警，继续使用（跟随上游最新）。
verify() {
    local file="$1" want="$2" strict="${3:-1}"
    [ -n "${want}" ] || die "没有 $(basename "$file") 的记录哈希，拒绝安装未校验的二进制"
    local got
    got="$(sha256sum "$file" | cut -d' ' -f1)"
    if [ "${got}" != "${want}" ]; then
        if [ "${strict}" -eq 1 ]; then
            cat >&2 <<EOF
错误：$(basename "$file") 的哈希与记录不符，已删除。

  期望：${want}
  实际：${got}

上游很可能重新构建过。请人工确认这份二进制可信，然后把脚本里对应的
SHA256 变量更新成上面那个「实际」值。
EOF
            rm -f "$file"
            exit 1
        fi
        warn "$(basename "$file") 的哈希已变化，继续使用（跟随 BtbN master-latest）"
        echo "    记录的构建：${FFMPEG_BUILD}" >&2
        echo "    期望：${want}" >&2
        echo "    实际：${got}" >&2
    fi
}

fetch() {
    local url="$1" out="$2"
    if [ -e "${out}" ] && [ "${FORCE}" -eq 0 ]; then
        echo "    已存在，跳过（--force 可重新下载）"
        return 0
    fi
    echo "    下载 ${url}"
    curl -fL --retry 3 --connect-timeout 20 -o "${out}.part" "${url}" \
        || die "下载失败：${url}"
    mv "${out}.part" "${out}"
}

# ---- yt-dlp -----------------------------------------------------------
# 打好的可执行文件含 PyInstaller/GPLv3+ 组件，整体按 GPLv3+ 分发（源码是
# Unlicense）。详细许可见 Client/tools/README.md。
fetch_ytdlp() {
    local asset url
    case "$(uname -s)" in
        Linux)  asset="yt-dlp_linux"; url="https://github.com/yt-dlp/yt-dlp/releases/download/${YTDLP_VERSION}/yt-dlp_linux" ;;
        Darwin) asset="yt-dlp_macos"; url="https://github.com/yt-dlp/yt-dlp/releases/download/${YTDLP_VERSION}/yt-dlp_macos" ;;
        *)      asset="yt-dlp.exe";   url="https://github.com/yt-dlp/yt-dlp/releases/download/${YTDLP_VERSION}/yt-dlp.exe" ;;
    esac
    mkdir -p "${DEST}/yt-dlp"
    echo "==> yt-dlp ${YTDLP_VERSION} (${asset})"
    fetch "${url}" "${DEST}/yt-dlp/${asset}"
    # Windows/macOS 的哈希本仓库没记录，只有 Linux 那份经过验证
    if [ "${asset}" = "yt-dlp_linux" ]; then
        verify "${DEST}/yt-dlp/${asset}" "${YTDLP_SHA256_linux_x86_64}"
    else
        warn "未校验 ${asset} 的哈希（本仓库只验证过 linux-x86_64 那份）"
    fi
    chmod +x "${DEST}/yt-dlp/${asset}" 2>/dev/null || true
}

# ---- ffmpeg / ffprobe -------------------------------------------------
# 只要这两个。ffplay 用不上，不放。
fetch_ffmpeg() {
    local tag archive url tmp
    case "$(uname -s)" in
        Linux) tag="linux64" ;;
        *)     tag="win64" ;;
    esac
    archive="ffmpeg-master-latest-${tag}-lgpl.tar.xz"
    url="https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/${archive}"

    mkdir -p "${DEST}/ffmpeg"
    echo "==> ffmpeg (${archive})"
    tmp="$(mktemp -d)"
    trap 'rm -rf "${tmp}"' RETURN

    fetch "${url}" "${tmp}/${archive}"

    # 解出来是 ffmpeg-master-latest-<tag>-lgpl/bin/{ffmpeg,ffprobe,ffplay}
    tar -xJf "${tmp}/${archive}" -C "${tmp}"
    local bin_dir
    bin_dir="$(find "${tmp}" -type d -name bin | head -1)"
    [ -n "${bin_dir}" ] || die "压缩包里没找到 bin/ 目录"

    local exe=""
    [ "${tag}" = "win64" ] && exe=".exe"

    for name in ffmpeg ffprobe; do
        cp "${bin_dir}/${name}${exe}" "${DEST}/ffmpeg/${name}${exe}"
    done
    # 许可证必须随包分发
    local lic
    lic="$(find "${tmp}" -name LICENSE.txt | head -1)"
    [ -n "${lic}" ] && cp "${lic}" "${DEST}/ffmpeg/LICENSE.txt"

    if [ "${tag}" = "linux64" ]; then
        # 倒数第二个参数 0 = 哈希不符只告警。ffmpeg 跟随 master-latest，
        # 上游重新构建是常态，不能因此让 CI 失败。
        verify "${DEST}/ffmpeg/ffmpeg"  "${FFMPEG_SHA256_linux_x86_64_ffmpeg}"  0
        verify "${DEST}/ffmpeg/ffprobe" "${FFMPEG_SHA256_linux_x86_64_ffprobe}" 0
    else
        warn "未校验 ${tag} 版 ffmpeg 的哈希（本仓库只验证过 linux64 那份）
    这同时说明：那份二进制尚未在本仓库的构建里验证过，Windows 支持也未实测。"
    fi
    chmod +x "${DEST}/ffmpeg/"* 2>/dev/null || true
}

echo "目标目录：${DEST}"
fetch_ytdlp
fetch_ffmpeg

echo
echo "完成。"
if [ -x "${DEST}/ffmpeg/ffmpeg" ]; then
    echo "  ffmpeg : $("${DEST}/ffmpeg/ffmpeg" -version 2>/dev/null | head -1)"
fi
if [ -x "${DEST}/yt-dlp/yt-dlp_linux" ]; then
    echo "  yt-dlp : $("${DEST}/yt-dlp/yt-dlp_linux" --version 2>/dev/null)"
fi
