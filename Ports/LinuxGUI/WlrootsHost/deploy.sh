#!/usr/bin/env bash
# Deploy serenity-wlroots-host into the VM at /serenity/bin.
#
# The bridge is owned by serenitid (serenity-desktop.service), which restarts
# it ~3.2s after a kill. A running binary cannot be overwritten ("Text file
# busy"), so we stage the new file on the SAME filesystem, kill every process
# whose /proc/*/exe points at the bridge, atomically mv the staged file into
# place, and let serenitid bring it back up.
#
# WARNING: restarting the bridge drops every Wayland client connection.
# Xwayland reconnects, but already-open GTK apps exit. Deploy when the session
# is idle. See Ports/GTKSerenity/WS_MENU_MAPPING.md §10.
#
# Build first (host):
#   PATH="$(printf '%s' "$PATH" | tr ':' '\n' | grep -v safe-bin | tr '\n' ':' | sed 's/:$//')" \
#   LIBRARY_PATH=<repo>/Build/vm-libs \
#   ninja -C <repo>/Build/lagom serenity-wlroots-host
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BIN="${1:-$ROOT/Build/lagom/bin/serenity-wlroots-host}"
SSH="$ROOT/Ports/GTKSerenity/vm-ssh.sh"
HOST="serenity@127.0.0.1"
KEY=/mnt/user/serenity/Build/vm/id_ed25519
SCP_ARGS=(-F /dev/null -i "$KEY" -P 2222
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
    -o BatchMode=yes -o ConnectTimeout=10)

[[ -x "$BIN" ]] || { echo "error: not executable: $BIN" >&2; exit 1; }
[[ -x "$SSH" ]] || { echo "error: missing $SSH" >&2; exit 1; }

EXPECTED="$(md5sum "$BIN" | cut -d' ' -f1)"
echo "== bridge binary =="
echo "   $BIN"
echo "   md5=$EXPECTED  size=$(stat -c %s "$BIN")"

# scp must be SERIAL here: two concurrent scp connections to this VM get
# "Connection closed" (sshd MaxStartups). Do not background or parallelize.
echo "== scp binary -> VM:/tmp/wlhost-deploy.new =="
scp "${SCP_ARGS[@]}" "$BIN" "$HOST:/tmp/wlhost-deploy.new" >/dev/null
STAGED="$("$SSH" "$HOST" 'md5sum /tmp/wlhost-deploy.new | cut -d" " -f1')"
[[ "$STAGED" == "$EXPECTED" ]] || { echo "error: staged md5 mismatch: $STAGED" >&2; exit 1; }
echo "   staged md5 verified"

echo "== stage + kill + atomic mv (avoids Text file busy) =="
"$SSH" "$HOST" 'sudo -n bash -s' <<'EOF'
cp /tmp/wlhost-deploy.new /serenity/bin/.wlhost-deploy-new
i=0
while [ $i -lt 8 ]; do
    P=""
    for p in /proc/[0-9]*; do
        [ -e "$p/exe" ] || continue
        readlink "$p/exe" 2>/dev/null | grep -q serenity-wlroots-host && P="$P ${p#/proc/}"
    done
    if [ -n "$P" ]; then kill -9 $P 2>/dev/null || true; fi
    mv -f /serenity/bin/.wlhost-deploy-new /serenity/bin/serenity-wlroots-host 2>/dev/null || true
    sleep 0.4
    i=$((i + 1))
done
chmod 755 /serenity/bin/serenity-wlroots-host
EOF

echo "== installed md5 =="
INSTALLED="$("$SSH" "$HOST" 'md5sum /serenity/bin/serenity-wlroots-host | cut -d" " -f1')"
echo "   $INSTALLED"
[[ "$INSTALLED" == "$EXPECTED" ]] || { echo "error: installed md5 mismatch" >&2; exit 1; }

echo "== waiting for serenitid to restart the bridge =="
READY=0
for _ in $(seq 1 30); do
    STATE="$("$SSH" "$HOST" '
        n=$(ps -eo args | grep -c "^/serenity/bin/serenity-wlroots-host" || true)
        s=0; [ -S /run/user/1000/wayland-serenity-0 ] && s=1
        echo "$n $s"' 2>/dev/null || echo "0 0")"
    NPROC="${STATE%% *}"
    SOCK="${STATE##* }"
    if [[ "$NPROC" -ge 1 && "$SOCK" == 1 ]]; then READY=1; break; fi
    sleep 1
done
[[ "$READY" == 1 ]] || { echo "error: bridge did not come back" >&2; exit 1; }

echo "== health =="
"$SSH" "$HOST" '
    echo "--- process ---"
    ps -eo pid,etime,args | grep "^ *[0-9].*serenity-wlroots-host" | grep -v grep || true
    echo "--- sockets ---"
    ls -l /run/user/1000/wayland-serenity-0 /tmp/.X11-unix/X100 2>/dev/null || true
    echo "--- menu global ---"
    grep "ws-menu: serenity_menu_manager_v1 global" /tmp/serenity-session.log | tail -1 || true
    echo "--- ready line ---"
    grep "wlroots-host ready" /tmp/serenity-session.log | tail -1 || true'
echo "== deploy OK =="
