#!/bin/bash
# 集成测试：把 labwc/Xwayland 里真实 Linux GUI 程序的顶层窗口喂给新桥。
#
# 前提：VM 里已装 labwc / gedit / gnome-text-editor / xterm / xwininfo，
#       且新桥已 cmake --install 到某个前缀（默认 /serenity）。
# 用法：PREFIX=/serenity bash tests/run_gui_smoke.sh
#
# 注意：这是「数据接入测试」——用真实窗口的 xid / 标题 / 尺寸驱动 Bridge 的
# on_wlr_* 入口，验证 ID 映射、参数翻译、状态同步与焦点决定转发。
# 新桥目前还没接进桌面启动链，也不是合成器；它不负责把窗口画出来。
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PREFIX=${PREFIX:-/serenity}
HARNESS=/tmp/wswlr-gui-smoke
RUNTIME=/tmp/wl-wswlr-gui

g++ -std=c++17 -Wall -Wextra -I"$PREFIX/include" "$SCRIPT_DIR/gui_smoke.cpp" \
    -L"$PREFIX/lib" -lws-wlr-bridge -o "$HARNESS"
echo "harness built against $PREFIX/lib/libws-wlr-bridge.a"

rm -rf "$RUNTIME"; mkdir -p "$RUNTIME"; chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Serenity

ls /tmp/.X11-unix/ | tr -d X | sort > /tmp/wswlr-x-before.txt

timeout 100 dbus-run-session -- bash -c '
  set -e
  export XDG_RUNTIME_DIR='"$RUNTIME"'
  labwc >/tmp/wswlr-gui-labwc.log 2>&1 &
  LP=$!
  sleep 4
  export WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -E "^wayland-[0-9]+$" | head -1)
  ls /tmp/.X11-unix/ | tr -d X | sort > /tmp/wswlr-x-after.txt
  NEWX=$(comm -13 /tmp/wswlr-x-before.txt /tmp/wswlr-x-after.txt | head -1)
  [ -z "$NEWX" ] && NEWX=$(tail -1 /tmp/wswlr-x-after.txt)
  export DISPLAY=":$NEWX"
  echo "labwc pid=$LP  WAYLAND_DISPLAY=$WAYLAND_DISPLAY  DISPLAY=$DISPLAY"

  # 强制 X11 后端，才能在 Xwayland root tree 里拿到真实顶层窗口与标题
  GDK_BACKEND=x11 gedit >/tmp/wswlr-gui-gedit.log 2>&1 &
  xterm -T "XTerm Bridge Test" -e sleep 60 >/tmp/wswlr-gui-xterm.log 2>&1 &
  GDK_BACKEND=x11 gnome-text-editor >/tmp/wswlr-gui-gte.log 2>&1 &
  sleep 8

  echo "--- 真实 Linux GUI 顶层窗口 ---"
  python3 '"$SCRIPT_DIR"'/gui_windows.py | tee /tmp/wswlr-windows.tsv
  echo "--- 喂进新桥 ---"
  '"$HARNESS"' /tmp/wswlr-windows.tsv
  kill $LP 2>/dev/null || true
'
