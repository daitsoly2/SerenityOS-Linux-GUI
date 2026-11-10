#!/usr/bin/env bash
# Verify a GTK4 app renders with Serenity control-level styling on the VM.
# Launches the app under Xwayland (:100), confirms it loaded our patched
# libgtk-4 from /serenity/gtk, grabs the VM framebuffer via VNC, converts to
# PNG, and prints the library map evidence.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SSH="./vm-ssh.sh"
APP="${1:-gnome-text-editor}"
OUT="${2:-gtk4-shot}"
PPM="/tmp/$OUT.ppm"
PNG="$HERE/$OUT.png"

echo "== launch $APP on VM (DISPLAY=:100) =="
"$SSH" serenity@127.0.0.1 "pkill -f '$(basename "$APP")' 2>/dev/null; sleep 0.5; setsid env DISPLAY=:100 SERENITY_PREFIX=/serenity/gtk XDG_DATA_DIRS=/serenity/gtk/share:/usr/share:/usr/local/share XDG_RUNTIME_DIR=/run/user/1000 $APP >/tmp/$OUT.log 2>&1 </dev/null & echo PID=\$!"
sleep 5
# NOTE: Linux truncates comm to 15 chars, so `pgrep -x gnome-text-editor`
# never matches. Use -f (full cmdline) instead.
PID="$("$SSH" serenity@127.0.0.1 "pgrep -f '$(basename "$APP")' | head -1")"
echo "app pid on VM: ${PID:-MISSING}"
if [ -z "$PID" ]; then
  echo "!! app did not start; tail log:"; "$SSH" serenity@127.0.0.1 "tail -20 /tmp/$OUT.log" | grep -v Permanently
  exit 1
fi
echo "== which libgtk-4 did it load? =="
"$SSH" serenity@127.0.0.1 "grep -oE '/serenity/gtk/[^ ]*libgtk-4[^ ]*' /proc/$PID/maps | head -1; grep -c '/serenity/gtk/lib' /proc/$PID/maps" | grep -v Permanently

echo "== grab VM framebuffer via VNC =="
python3 /mnt/user/serenity/Build/vm/vncshot.py "shot:$PPM"
echo "saved $PPM"
"$SSH" serenity@127.0.0.1 "pkill -f '$(basename "$APP")' 2>/dev/null || true"
convert "$PPM" "$PNG"
echo "== wrote $PNG =="
ls -la "$PNG"
