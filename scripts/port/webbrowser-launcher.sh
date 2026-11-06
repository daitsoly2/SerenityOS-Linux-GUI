#!/bin/sh
# Serenity 的"浏览器"：用宿主 Firefox 当引擎，X11 桥把窗口映射成 Serenity 窗口。
#
# LibWeb 是自研引擎，兼容性不足以日常使用；换内核不现实（等于重写浏览器），
# 所以这里走"换浏览器"：Firefox 跑在 Xvfb 上，由 serenity-x11-bridge 变成
# Serenity 窗口（和 Wine 应用同一条路径）。
#
# Installed to <prefix>/bin/serenity-webbrowser by 05-install-prefix.sh.

export DISPLAY=${DISPLAY:-:100}
export MOZ_ENABLE_WAYLAND=0

browser=$(command -v firefox-esr || command -v firefox || true)
if [ -z "$browser" ]; then
    echo "serenity-webbrowser: firefox not installed (apt install firefox-esr)" >&2
    exit 127
fi

exec "$browser" --no-remote "$@"
