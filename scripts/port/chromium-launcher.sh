#!/bin/sh
# Serenity 专属 Chromium：使用 Debian Chromium 内核，在原生 Wayland 下运行。
# 不依赖 Xvfb，会由 Serenity/wlroots 合成器套上 Serenity 窗框。

set -eu

export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-serenity-0}"
export XDG_CURRENT_DESKTOP=Serenity
export GTK_CSD=0
export MOZ_ENABLE_WAYLAND=1

profile="${XDG_CONFIG_HOME:-$HOME/.config}/serenity-chromium"
mkdir -p "$profile"

exec chromium \
    --ozone-platform=wayland \
    --lang=zh-CN \
    --user-data-dir="$profile" \
    --no-first-run \
    --disable-session-crashed-bubble \
    --disable-features=ChromeWhatsNewUI \
    "$@"
