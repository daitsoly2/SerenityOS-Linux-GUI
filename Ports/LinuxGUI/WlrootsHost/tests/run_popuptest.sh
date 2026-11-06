#!/bin/sh
# popup configure 契约验证：起 wh2（wlroots host），跑 popuptest，收集双方日志。
# 环境取自 serenity 会话进程（XDG_RUNTIME_DIR=/run/user/1000, APP_DISPLAY=:100）。
export XDG_RUNTIME_DIR=/run/user/1000
export SERENITY_APP_DISPLAY=:100
export DISPLAY=:100
export SERENITY_RESOURCE_ROOT=/res
export SERENITY_BIN=/serenity/bin
export SERENITY_WL_SOCKET=wayland-serenity-9
export SERENITY_XWAYLAND_DISPLAY=${SERENITY_XWAYLAND_DISPLAY:-:101}
export WAYLAND_DISPLAY=$SERENITY_WL_SOCKET

pkill -x wh2 2>/dev/null
sleep 0.3
rm -f "/run/user/1000/$SERENITY_WL_SOCKET"

/tmp/wh2 >/tmp/wh2.log 2>&1 &
HPID=$!

i=0
while [ $i -lt 60 ]; do
    [ -S "/run/user/1000/$SERENITY_WL_SOCKET" ] && break
    sleep 0.1
    i=$((i + 1))
done
if [ ! -S "/run/user/1000/$SERENITY_WL_SOCKET" ]; then
    echo "HOST_FAIL: socket never appeared"
    tail -20 /tmp/wh2.log
    kill $HPID 2>/dev/null
    exit 1
fi
echo "socket ready after ${i}00ms"

/tmp/popuptest >/tmp/popuptest.log 2>&1
RC=$?
echo "popuptest rc=$RC"

if kill -0 $HPID 2>/dev/null; then
    echo "HOST_ALIVE=1"
else
    echo "HOST_ALIVE=0 (host died)"
fi
sleep 0.3
kill $HPID 2>/dev/null
wait $HPID 2>/dev/null
echo "---- popuptest stdout/stderr ----"
cat /tmp/popuptest.log
exit $RC
