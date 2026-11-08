#!/usr/bin/env bash
# 极简 text-input-v3 测试客户端（无 GUI）：映射一个窗口 → 点进去 → enable IME，
# 把引擎回来的 preedit/commit_string 全部打印出来。用来验证桥内建输入法，
# 不依赖 gedit/wordcraft 这些真实应用。
#
# 用法：build-titest.sh && scp titest 到 VM → WAYLAND_DISPLAY=wayland-serenity-0 ./titest
set -euo pipefail
cd "$(dirname "$0")"
PROTO=/usr/share/wayland-protocols
OUT=build
mkdir -p "$OUT"
wayland-scanner client-header "$PROTO/stable/xdg-shell/xdg-shell.xml" "$OUT/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$PROTO/stable/xdg-shell/xdg-shell.xml" "$OUT/xdg-shell-protocol.c"
wayland-scanner client-header "$PROTO/unstable/text-input/text-input-unstable-v3.xml" "$OUT/text-input-unstable-v3-client-protocol.h"
wayland-scanner private-code  "$PROTO/unstable/text-input/text-input-unstable-v3.xml" "$OUT/text-input-protocol.c"
cc -O1 -g -I"$OUT" titest.c "$OUT/xdg-shell-protocol.c" "$OUT/text-input-protocol.c" \
   $(pkg-config --cflags --libs wayland-client) -o "$OUT/titest"
echo "built $OUT/titest"
