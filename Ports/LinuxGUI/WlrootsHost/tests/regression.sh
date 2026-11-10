#!/usr/bin/env bash
# Post-deploy regression suite for serenity-wlroots-host (run on the HOST).
#
# Verifies both the new serenity_menu_v1 path AND that the existing desktop
# still works (Wayland event loop, Xwayland/X11 apps, GTK3, GTK4).
# Results go to deploy-test/results.tsv; screenshots to deploy-test/*.png.
#
# Prerequisite: ./deploy.sh has been run, Xwayland has reconnected.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SSH="$ROOT/Ports/GTKSerenity/vm-ssh.sh"
HOST="serenity@127.0.0.1"
VNC=/mnt/user/serenity/Build/vm/vncshot.py
KEY=/mnt/user/serenity/Build/vm/id_ed25519
SCP_ARGS=(-F /dev/null -i "$KEY" -P 2222
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
    -o BatchMode=yes -o ConnectTimeout=10)
OUT="$ROOT/deploy-test"
mkdir -p "$OUT"
: > "$OUT/results.tsv"

record() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" | tee -a "$OUT/results.tsv"; }

vm() { "$SSH" "$HOST" "$@" 2>/dev/null; }

echo "== build test clients =="
gcc -I"$HERE" -o "$OUT/wl-roundtrip" "$HERE/tests/wl-roundtrip.c" -lwayland-client || { echo "build wl-roundtrip FAILED"; exit 1; }
gcc -I"$HERE" -o "$OUT/menu-probe" "$HERE/tests/menu-probe.c" \
    "$HERE/protocol/serenity-menu-v1-client.c" -lwayland-client || { echo "build menu-probe FAILED"; exit 1; }

echo "== transfer (serial scp) =="
vm 'mkdir -p /tmp/serenity-regress'
scp "${SCP_ARGS[@]}" "$OUT/wl-roundtrip" "$HOST:/tmp/serenity-regress/wl-roundtrip" >/dev/null
scp "${SCP_ARGS[@]}" "$OUT/menu-probe" "$HOST:/tmp/serenity-regress/menu-probe" >/dev/null
vm 'chmod +x /tmp/serenity-regress/*'

ENV='export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-serenity-0'

# ---- T1/T2: bridge + sockets -------------------------------------------
STATE="$(vm 'n=$(ps -eo args | grep -c "^/serenity/bin/serenity-wlroots-host" || true); w=0; x=0; [ -S /run/user/1000/wayland-serenity-0 ] && w=1; [ -S /tmp/.X11-unix/X100 ] && x=1; echo "$n $w $x"')"
read -r NPROC WSOCK XSOCK <<< "$STATE"
if [[ "$NPROC" -ge 1 ]]; then record T1 PASS "桥进程" "serenitid 托管, $NPROC 实例"; else record T1 FAIL "桥进程" "未运行"; fi
if [[ "$WSOCK" == 1 && "$XSOCK" == 1 ]]; then record T2 PASS "Wayland + X11 socket" "两者均存在"; else record T2 FAIL "Wayland + X11 socket" "w=$WSOCK x=$XSOCK"; fi

# ---- T3: bridge event loop (cold-start deadlock guard) -------------------
R3="$(vm "$ENV; timeout 10 /tmp/serenity-regress/wl-roundtrip 2>&1 | head -3")"
if echo "$R3" | grep -q 'ROUNDTRIP_OK globals=1[0-9]'; then
    record T3 PASS "Wayland 事件循环 roundtrip" "$R3"
else
    record T3 FAIL "Wayland 事件循环 roundtrip" "${R3:-no output}"
fi

# ---- T4..T7: serenity_menu_v1 closed loop --------------------------------
vm 'pkill -9 -x menu-probe 2>/dev/null; : > /tmp/serenity-regress/probe.log; true'
vm "$ENV SERENITY_MENU_HOLD_SECONDS=25 SERENITY_MENU_X=300 SERENITY_MENU_Y=200; setsid /tmp/serenity-regress/menu-probe >/tmp/serenity-regress/probe.log 2>&1 </dev/null & sleep 2; cat /tmp/serenity-regress/probe.log"
PLOG="$(vm 'cat /tmp/serenity-regress/probe.log')"
if echo "$PLOG" | grep -q 'BOUND name='; then
    record T4 PASS "serenity_menu_v1 global / bind" "$(echo "$PLOG" | grep BOUND)"
else
    record T4 FAIL "serenity_menu_v1 global / bind" "${PLOG:-no output}"
fi

# screenshot while the native menu is up
( cd "$(dirname "$VNC")" && python3 "$VNC" "shot:$OUT/menu-native.ppm" >/dev/null 2>&1 )
if command -v convert >/dev/null && [[ -f "$OUT/menu-native.ppm" ]]; then
    convert "$OUT/menu-native.ppm" "$OUT/menu-native.png"
    python3 -c 'import os,sys; os.remove(sys.argv[1])' "$OUT/menu-native.ppm" 2>/dev/null || true
fi

BRIDGE="$(vm 'grep "ws-menu:" /tmp/serenity-session.log | tail -14')"
if echo "$BRIDGE" | grep -q 'popup -> Menu.cpp'; then
    record T5 PASS "菜单由 WindowServer/Menu.cpp 弹出" "$(echo "$BRIDGE" | grep 'popup ->' | tail -1)"
else
    record T5 FAIL "菜单由 WindowServer/Menu.cpp 弹出" "无 popup 日志"
fi

# keyboard activation (Down + Return) — coordinate independent
( cd "$(dirname "$VNC")" && python3 -c "
import vncshot, struct, time
s = vncshot.vnc_handshake()
def key(down, sym): s.sendall(struct.pack('>BBHI', 4, 1 if down else 0, 0, sym))
key(1, 0xff54); time.sleep(0.2); key(0, 0xff54); time.sleep(0.4)
key(1, 0xff0d); time.sleep(0.2); key(0, 0xff0d); time.sleep(0.6)
s.close()
" >/dev/null 2>&1 )

ACT="$(vm 'grep "ACTIVATED" /tmp/serenity-regress/probe.log | tail -1')"
BACT="$(vm 'grep "ws-menu: activated" /tmp/serenity-session.log | tail -1')"
if [[ -n "$ACT" && -n "$BACT" ]]; then
    record T6 PASS "激活回传 (Action -> 协议事件)" "$ACT / $BACT"
else
    record T6 FAIL "激活回传" "client=${ACT:-none} bridge=${BACT:-none}"
fi

# wait for the probe to dismiss itself and finish
for _ in $(seq 1 30); do
    vm 'grep -q DONE /tmp/serenity-regress/probe.log' && break
    sleep 1
done
DIS="$(vm 'grep -E "DISMISSED|DONE" /tmp/serenity-regress/probe.log | tr "\n" " "')"
if echo "$DIS" | grep -q DISMISSED; then
    record T7 PASS "dismiss 回传与资源清理" "$DIS"
else
    record T7 FAIL "dismiss 回传与资源清理" "${DIS:-no output}"
fi

# ---- T8: X11 (Xwayland) app ---------------------------------------------
# xwininfo shows WM_NAME ("serenity@host: ~") and the class ("xterm" "XTerm");
# the -title value only appears on the XWM/wl_surface side, so match the class.
vm 'pkill -9 -x xterm 2>/dev/null; DISPLAY=:100 setsid xterm -title "REGRESS-X11" >/tmp/serenity-regress/xterm.log 2>&1 </dev/null & sleep 3; true'
X11W="$(vm 'DISPLAY=:100 xwininfo -root -tree 2>/dev/null | grep -ci "xterm"')"
XWMPAIR="$(vm 'grep -c "paired X .*REGRESS-X11" /tmp/serenity-session.log | tail -1')"
if [[ "${X11W:-0}" -ge 1 ]]; then
    record T8 PASS "X11/Xwayland 应用" "xterm 窗口 $X11W 个 / XWM 配对 ${XWMPAIR:-0} 条"
else
    record T8 FAIL "X11/Xwayland 应用" "xwininfo 无 xterm 窗口; log=$(vm 'head -5 /tmp/serenity-regress/xterm.log')"
fi

# ---- T9: GTK3 app --------------------------------------------------------
vm 'pkill -9 -x gedit 2>/dev/null; export DISPLAY=:100 WAYLAND_DISPLAY=wayland-serenity-0 XDG_RUNTIME_DIR=/run/user/1000 SERENITY_PREFIX=/serenity/gtk; setsid gedit >/tmp/serenity-regress/gedit.log 2>&1 </dev/null & sleep 5; pidof gedit'
G3="$(vm 'pidof gedit || echo NONE')"
if [[ "$G3" != NONE ]]; then record T9 PASS "GTK3 gedit 启动" "pid=$G3"; else record T9 FAIL "GTK3 gedit 启动" "$(vm 'head -5 /tmp/serenity-regress/gedit.log')"; fi

# ---- T10: GTK4 app -------------------------------------------------------
vm 'pkill -9 -x gnome-text-editor 2>/dev/null; export DISPLAY=:100 WAYLAND_DISPLAY=wayland-serenity-0 XDG_RUNTIME_DIR=/run/user/1000 SERENITY_PREFIX=/serenity/gtk GTK_THEME=Serenity GTK_CSD=0 GDK_BACKEND=wayland DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus; setsid gnome-text-editor >/tmp/serenity-regress/gte.log 2>&1 </dev/null & sleep 6; pidof gnome-text-editor'
G4="$(vm 'pidof gnome-text-editor || echo NONE')"
if [[ "$G4" != NONE ]]; then record T10 PASS "GTK4 gnome-text-editor 启动" "pid=$G4"; else record T10 FAIL "GTK4 gnome-text-editor 启动" "$(vm 'head -8 /tmp/serenity-regress/gte.log')"; fi

# ---- final desktop screenshot -------------------------------------------
( cd "$(dirname "$VNC")" && python3 "$VNC" "shot:$OUT/desktop.ppm" >/dev/null 2>&1 )
if command -v convert >/dev/null && [[ -f "$OUT/desktop.ppm" ]]; then
    convert "$OUT/desktop.ppm" "$OUT/desktop.png"
    python3 -c 'import os,sys; os.remove(sys.argv[1])' "$OUT/desktop.ppm" 2>/dev/null || true
    record T11 PASS "桌面截图" "deploy-test/desktop.png"
else
    record T11 FAIL "桌面截图" "vncshot 未产出"
fi

echo
echo "== summary =="
awk -F'\t' '{c[$2]++} END {printf "PASS=%d FAIL=%d\n", c["PASS"], c["FAIL"]}' "$OUT/results.tsv"
column -t -s $'\t' "$OUT/results.tsv" 2>/dev/null || cat "$OUT/results.tsv"
