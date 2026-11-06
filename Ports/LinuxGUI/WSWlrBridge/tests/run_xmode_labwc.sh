#!/bin/bash
# labwc 混合模式测试（可一键回滚）。
#
#   labwc (DRM, via seatd) -> Xwayland -> Serenity 桌面（X 客户端，全屏无边框）
#                          -> 外来 GTK/Qt 应用（labwc 原生 Wayland 客户端）
#                          -> serenity-ws-wlr-bridge-host（labwc 的 Wayland 客户端，桥接到 Serenity）
#
# 与 Build/vm/xmode_start.sh 的区别：
#   * 不覆盖 /serenity/bin/serenity-window-server（生产二进制已支持
#     SERENITY_PRESENTER=x11，用 VM 内旧构建覆盖会降级并影响回滚后的会话）；
#   * 用新桥 host 取代已移除的 serenity-toplevel-bridge；
#   * 外来应用走 labwc 原生 Wayland，专门验证「更兼容 Linux 应用」；
#   * stop 子命令完整恢复 KMS 桌面会话。
#
# 用法：bash tests/run_xmode_labwc.sh start|status|stop
set -u

RUNTIME=/run/user/1000
BIN=/serenity/bin
STATE=${WSWLR_STATE_PATH:-/tmp/serenity-ws-wlr-windows}
CMD=${WSWLR_CMD_PATH:-/tmp/serenity-ws-wlr-cmd}
EVENTS=${WSWLR_EVENT_PATH:-/tmp/serenity-ws-wlr-events}

start()
{
    export XDG_RUNTIME_DIR="$RUNTIME"
    export SERENITY_BIN="$BIN" SERENITY_PREFIX=/serenity SERENITY_RESOURCE_ROOT=/res
    export LD_LIBRARY_PATH=/serenity/lib
    export WSWLR_STATE_PATH="$STATE" WSWLR_CMD_PATH="$CMD" WSWLR_EVENT_PATH="$EVENTS"
    rm -f "$STATE" "$CMD" "$EVENTS" /tmp/xmode-wslabr-*.log

    echo "=== 1. 释放 DRM（停正常 KMS 会话）==="
    sudo systemctl stop serenity-desktop.service
    sleep 2
    pkill -f "/serenity/bin/serenity-window-serve[r]" 2>/dev/null || true
    pkill -x serenity-shell 2>/dev/null || true
    pkill -f "/serenity/bin/serenity-porta[l]" 2>/dev/null || true
    pkill -f "/serenity/bin/serenity-config-serve[r]" 2>/dev/null || true
    pkill -f "/serenity/bin/serenity-launch-serve[r]" 2>/dev/null || true
    pkill -f "/serenity/bin/serenity-clipboard-serve[r]" 2>/dev/null || true
    pkill -x labwc 2>/dev/null || true
    pkill -f "serenity-ws-wlr-bridge-hos[t]" 2>/dev/null || true
    sleep 1

    EXISTING_X=$(ls /tmp/.X11-unix/ 2>/dev/null | tr -d X | tr '\n' ' ')
    echo "=== 2. 启动 labwc（DRM + seatd）==="
    export LIBSEAT_BACKEND=seatd WLR_BACKENDS=drm WLR_RENDERER=pixman
    export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Serenity
    unset WAYLAND_DISPLAY DISPLAY
    setsid labwc >/tmp/xmode-wslabr-labwc.log 2>&1 </dev/null &
    sleep 5
    WLD=$(ls "$RUNTIME" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
    [ -z "$WLD" ] && { echo "labwc 没起来:"; tail -20 /tmp/xmode-wslabr-labwc.log; stop; exit 1; }
    export WAYLAND_DISPLAY="$WLD"
    echo "labwc WAYLAND_DISPLAY=$WAYLAND_DISPLAY"

    echo "=== 3. 找 labwc 新建的 Xwayland socket ==="
    XDISP=""
    for _ in $(seq 1 40); do
        for candidate in $(ls /tmp/.X11-unix/ 2>/dev/null | tr -d X); do
            case " $EXISTING_X " in *" $candidate "*) continue ;; esac
            XDISP=$candidate
        done
        [ -n "$XDISP" ] && break
        sleep 0.5
    done
    [ -z "$XDISP" ] && { echo "Xwayland 没起来"; stop; exit 1; }
    export DISPLAY=":$XDISP" SERENITY_APP_DISPLAY=":$XDISP"
    echo "Xwayland DISPLAY=$DISPLAY"

    echo "=== 4. Serenity 桌面作为 X 客户端 ==="
    export SERENITY_PRESENTER=x11 SERENITY_LOG=/tmp/xmode-wslabr-session.log GTK_THEME=Serenity
    for svc in serenity-config-server serenity-launch-server serenity-clipboard-server; do
        setsid "$BIN/$svc" >/tmp/xmode-wslabr-$svc.log 2>&1 </dev/null &
    done
    sleep 2
    setsid "$BIN/serenity-window-server" --config /etc/WindowServer.ini \
        >/tmp/xmode-wslabr-ws.log 2>&1 </dev/null &
    sleep 4
    setsid "$BIN/serenity-portal" >/tmp/xmode-wslabr-portal.log 2>&1 </dev/null &
    setsid "$BIN/serenity-shell" >/tmp/xmode-wslabr-shell.log 2>&1 </dev/null &
    sleep 4

    echo "=== 5. 新桥 host 接 labwc 的 foreign-toplevel ==="
    setsid "$BIN/serenity-ws-wlr-bridge-host" >/tmp/xmode-wslabr-host.log 2>&1 </dev/null &
    sleep 3

    echo "=== 6. 外来 Linux 应用（labwc 原生 Wayland）==="
    setsid env WAYLAND_DISPLAY="$WAYLAND_DISPLAY" GDK_BACKEND=wayland \
        gedit >/tmp/xmode-wslabr-gedit.log 2>&1 </dev/null &
    setsid env WAYLAND_DISPLAY="$WAYLAND_DISPLAY" GDK_BACKEND=wayland \
        gnome-text-editor >/tmp/xmode-wslabr-gte.log 2>&1 </dev/null &
    setsid env DISPLAY="$DISPLAY" xterm -T "XTerm labwc" -e sleep 90 \
        >/tmp/xmode-wslabr-xterm.log 2>&1 </dev/null &
    sleep 8

    status
}

status()
{
    echo "--- 进程 ---"
    echo "labwc=$(pgrep -x labwc | tr '\n' ' ')"
    echo "window-server=$(pgrep -f 'serenity-window-serve[r]' | tr '\n' ' ')"
    echo "shell=$(pgrep -x serenity-shell | tr '\n' ' ')"
    # pgrep -x 只匹配 15 字符的 comm，长进程名必须用 -f + [t] 防自匹配。
    echo "host=$(pgrep -f 'serenity-ws-wlr-bridge-hos[t]' | tr '\n' ' ')"
    echo "gedit=$(pgrep -x gedit | tr '\n' ' ') gte=$(pgrep -f 'gnome-text-edito[r]' | tr '\n' ' ') xterm=$(pgrep -x xterm | tr '\n' ' ')"
    echo "--- presenter ---"
    grep -E "presenter|X11|display mode" /tmp/xmode-wslabr-ws.log 2>/dev/null | head -4
    echo "--- 新桥发布的窗口列表 ---"
    cat "$STATE" 2>/dev/null || echo "(无)"
    echo "--- 新桥事件日志尾 ---"
    tail -8 "$EVENTS" 2>/dev/null || echo "(无)"
    echo "--- labwc 错误 ---"
    grep -E "ERROR|CRITICAL" /tmp/xmode-wslabr-labwc.log 2>/dev/null | head -5 || true
}

stop()
{
    echo "=== 回滚到正常 KMS 会话 ==="
    pkill -f "serenity-ws-wlr-bridge-hos[t]" 2>/dev/null || true
    pkill -x serenity-shell 2>/dev/null || true
    pkill -f "/serenity/bin/serenity-window-serve[r]" 2>/dev/null || true
    pkill -f "/serenity/bin/serenity-porta[l]" 2>/dev/null || true
    pkill -f "/serenity/bin/serenity-config-serve[r]" 2>/dev/null || true
    pkill -f "/serenity/bin/serenity-launch-serve[r]" 2>/dev/null || true
    pkill -f "/serenity/bin/serenity-clipboard-serve[r]" 2>/dev/null || true
    pkill -x gedit 2>/dev/null || true
    pkill -x gnome-text-editor 2>/dev/null || true
    pkill -x xterm 2>/dev/null || true
    pkill -x labwc 2>/dev/null || true
    sleep 3
    sudo systemctl start serenity-desktop.service
    sleep 5
    echo "serenity-desktop=$(systemctl is-active serenity-desktop.service)"
    echo "window-server=$(pgrep -c -f 'serenity-window-serve[r]' || true)"
}

case "${1:-status}" in
    start) start ;;
    status) status ;;
    stop) stop ;;
    *) echo "usage: $0 start|status|stop" >&2; exit 2 ;;
esac
