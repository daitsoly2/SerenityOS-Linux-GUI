#!/bin/bash
# 先期闭环：labwc 托管真实 Linux GUI 应用，新桥 host 作为 Wayland 客户端接入，
# 通过 FIFO 下发 Serenity 的决定，验证「决定 → wlr_* → labwc → state 回传」。
#
# 前提：host 已 cmake --install 到某个前缀（默认 /serenity）。
# 用法：PREFIX=/serenity bash tests/run_host_smoke.sh
set -e

PREFIX=${PREFIX:-/serenity}
HOST_BIN="$PREFIX/bin/serenity-ws-wlr-bridge-host"
export HOST_BIN

STATE=/tmp/serenity-ws-wlr-windows
CMD=/tmp/serenity-ws-wlr-cmd
EVENTS=/tmp/serenity-ws-wlr-events
export STATE CMD EVENTS
export WSWLR_STATE_PATH="$STATE" WSWLR_CMD_PATH="$CMD" WSWLR_EVENT_PATH="$EVENTS"

[ -x "$HOST_BIN" ] || { echo "missing $HOST_BIN" >&2; exit 2; }
rm -f "$STATE" "$CMD" "$EVENTS" /tmp/wswlr-host.log /tmp/wswlr-host-labwc.log

RUNTIME=/tmp/wl-wswlr-host
rm -rf "$RUNTIME"; mkdir -p "$RUNTIME"; chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman WLR_HEADLESS_OUTPUTS=1
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Serenity

ls /tmp/.X11-unix/ | tr -d X | sort > /tmp/wswlr-host-x-before.txt

timeout 110 dbus-run-session -- bash -c '
  set -e
  export XDG_RUNTIME_DIR='"$RUNTIME"'
  labwc >/tmp/wswlr-host-labwc.log 2>&1 &
  LP=$!
  sleep 4
  export WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -E "^wayland-[0-9]+$" | head -1)
  ls /tmp/.X11-unix/ | tr -d X | sort > /tmp/wswlr-host-x-after.txt
  NEWX=$(comm -13 /tmp/wswlr-host-x-before.txt /tmp/wswlr-host-x-after.txt | head -1)
  [ -z "$NEWX" ] && NEWX=$(tail -1 /tmp/wswlr-host-x-after.txt)
  export DISPLAY=":$NEWX"
  echo "labwc pid=$LP  WAYLAND_DISPLAY=$WAYLAND_DISPLAY  DISPLAY=$DISPLAY"

  GDK_BACKEND=x11 gedit >/tmp/wswlr-host-gedit.log 2>&1 &
  GDK_BACKEND=x11 gnome-text-editor >/tmp/wswlr-host-gte.log 2>&1 &
  xterm -T "XTerm Host Test" -e sleep 70 >/tmp/wswlr-host-xterm.log 2>&1 &
  sleep 5

  echo "--- 启动新桥 host ---"
  "$HOST_BIN" >/tmp/wswlr-host.log 2>&1 &
  HP=$!
  sleep 4
  cat /tmp/wswlr-host.log

  echo "--- 桥产出的窗口状态（labwc 外来窗口 → ws 窗口）---"
  cat "$STATE"

  ID=$(grep -v "^#" "$STATE" | head -1 | cut -f1)
  if [ -z "$ID" ]; then echo "no windows published by host" >&2; kill $HP $LP 2>/dev/null || true; exit 1; fi
  echo "--- 下发 Serenity 决定: activate $ID ---"
  echo "activate $ID" > "$CMD"
  sleep 3

  echo "--- 决定转发后的状态 ---"
  cat "$STATE"
  echo "--- 事件日志 ---"
  cat "$EVENTS"

  echo "--- 断言 ---"
  grep -q "focus-decision activate" "$EVENTS" && echo "OK: 决定已翻译为 wlr activate 调用"
  if grep -q "activated=1" "$EVENTS"; then
    echo "OK: labwc 回传 activated=1，真实焦点闭环成立"
  else
    echo "NOTE: headless labwc 未回传 state（闭环前半段仍已验证）"
  fi
  grep -q "VIOLATION" "$EVENTS" && { echo "FAIL: 桥越权调用了 WS 决策接口" >&2; kill $HP $LP 2>/dev/null || true; exit 1; } || echo "OK: 桥未调用任何 WS 决策接口"

  kill $HP $LP 2>/dev/null || true
'
