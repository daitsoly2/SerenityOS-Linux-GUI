#!/usr/bin/env bash
# 运行 Linux 版 WindowServer。
#
#   sudo ./03-run.sh                       # 自动选择显示后端
#   sudo ./03-run.sh --width 1600 --height 900
#   sudo SERENITY_KMSDRM=1 ./03-run.sh     # 直接走 DRM/KMS（无 X/Wayland）
#
# 无显示服务器时自动使用 SDL dummy 驱动，并输出一张 PPM 截图到
# Build/screenshot.ppm，用于验证移植是否跑通。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ROOT}/Build/lagom"
BIN="${BUILD}/bin/serenity-window-server"
RES="${BUILD}/Root/res"

if [[ ! -x "${BIN}" ]]; then
    echo "找不到 ${BIN}，请先运行 ./02-build.sh" >&2
    exit 1
fi

# Lagom 只往 Root/res 拷了 Liberation 字体；主题、光标、图标、Serenity 字体
# 都还在 Base/res，这里接进来。
mkdir -p "${RES}"
for d in apps color-palettes color-schemes cursor-themes emoji graphics html icons keymaps themes wallpapers; do
    if [[ -d "${ROOT}/Base/res/${d}" ]]; then
        ln -sfn "${ROOT}/Base/res/${d}" "${RES}/${d}"
    fi
done
if [[ -d "${ROOT}/Base/res/fonts" ]]; then
    mkdir -p "${RES}/fonts"
    cp -r "${ROOT}/Base/res/fonts/." "${RES}/fonts/" 2>/dev/null || true
fi

# WindowServer 以及 Serenity 的主题/字体代码会读取 /res。
if [[ ! -e /res ]]; then
    ln -s "${RES}" /res
    echo "已创建 /res -> ${RES}"
elif [[ ! -L /res ]]; then
    echo "警告：/res 已存在且不是软链接，主题与字体可能加载失败" >&2
fi

if [[ ! -f /etc/WindowServer.ini ]]; then
    touch /etc/WindowServer.ini
    echo "已创建空配置 /etc/WindowServer.ini"
fi

if [[ -n "${SERENITY_KMSDRM:-}" ]]; then
    echo "使用 SDL kmsdrm 后端"
    exec env SDL_VIDEODRIVER=kmsdrm "${BIN}" "$@"
fi

if [[ -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; then
    exec "${BIN}" "$@"
fi

echo "未检测到 DISPLAY / WAYLAND_DISPLAY，使用 headless 截图模式"
OUT="${ROOT}/Build/screenshot.ppm"
exec env SDL_VIDEODRIVER=dummy "${BIN}" \
    --screenshot "${OUT}" --screenshot-delay 1500 "$@"
