#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# 一条龙构建 Switch 的 .nro。
#
# 首次运行会自动准备工具链；之后直接调 make。
#
# 为什么需要这套绕路：
#   devkitPro 官方源 pkg.devkitpro.org 被 Cloudflare 拦截（403），
#   拦的依据是 **TLS 指纹**，不是 IP —— 所以光挂代理没用。
#   解法是 curl-impersonate（伪装 Chrome 的 TLS 指纹）+ 一个未被封的出口 IP，
#   两者缺一不可：只换 IP 仍被指纹规则拦，只换指纹仍被 IP 规则拦。
#
# 用法：
#   ./build.sh                    # 构建
#   ./build.sh --clean            # 先清理
#   PROXY=http://127.0.0.1:7897 ./build.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

DEVKITPRO_DIR="${DEVKITPRO_DIR:-$HOME/devkitpro}"
DEVKITPRO="$DEVKITPRO_DIR/root/opt/devkitpro"
CI_DIR="${CI_DIR:-$HOME/.cache/curl-impersonate}"

PROXY="${PROXY:-}"
CI_VERSION="2.2.3"
CI_URL="https://github.com/lexiforest/curl-impersonate/releases/download/v${CI_VERSION}/curl-impersonate-v${CI_VERSION}.x86_64-macos.tar.gz"

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m警告:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m错误:\033[0m %s\n' "$*" >&2; exit 1; }

# 代理不是必须的，但没有它 GitHub 会慢到无法接受
if [ -n "$PROXY" ]; then
  export HTTPS_PROXY="$PROXY" HTTP_PROXY="$PROXY" ALL_PROXY="$PROXY"
  info "使用代理 $PROXY"
fi

# ---- 1. curl-impersonate ----
if [ ! -x "$CI_DIR/curl_chrome136" ]; then
  info "下载 curl-impersonate（伪装 Chrome TLS 指纹，绕 Cloudflare）"
  mkdir -p "$CI_DIR"
  curl -sSL --max-time 300 -o "$CI_DIR/ci.tar.gz" "$CI_URL" \
    || die "curl-impersonate 下载失败（试试设置 PROXY）"
  tar xzf "$CI_DIR/ci.tar.gz" -C "$CI_DIR"
  rm -f "$CI_DIR/ci.tar.gz"
fi
export PATH="$CI_DIR:$PATH"

# ---- 2. devkitPro 工具链 ----
if [ ! -x "$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc" ]; then
  info "准备 devkitPro 工具链（首次约 5–10 分钟）"
  python3 "$REPO/tools/fetch-devkitpro.py" --dest "$DEVKITPRO_DIR" \
    --curl "$CI_DIR/curl_chrome136"
fi

export DEVKITPRO
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH"

info "工具链: $(aarch64-none-elf-gcc --version | head -1)"

# ---- 3. 构建 ----
cd "$HERE"
if [ "${1:-}" = "--clean" ]; then
  make clean
fi
info "构建 .nro"
make

NRO="$HERE/hongguo-probe.nro"
[ -f "$NRO" ] || die "构建结束但没有产出 .nro"

# NRO 的魔数在偏移 0x10（头部设计如此，不是文件开头）
MAGIC="$(dd if="$NRO" bs=1 skip=16 count=4 2>/dev/null)"
[ "$MAGIC" = "NRO0" ] || die "产物不是合法的 NRO0 文件"

info "完成: $NRO  ($(wc -c <"$NRO" | tr -d ' ') 字节)"
echo
echo "装到 Switch："
echo "  1. 把 hongguo-probe.nro 拷到 SD 卡的 switch/ 目录"
echo "  2. 从 hbmenu 启动"
