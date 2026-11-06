#!/usr/bin/env bash
# 启动 WindowServer，然后运行自带的 AnalogClock。
#
#   sudo ./04-run-app.sh
#
# WindowServer 在后台运行，app 退出后会一并结束。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ROOT}/Build/lagom"
WS="${BUILD}/bin/serenity-window-server"
APP="${BUILD}/bin/serenity-analog-clock"
RES="${BUILD}/Root/res"

if [[ ! -x "${WS}" || ! -x "${APP}" ]]; then
    echo "找不到 ${WS} 或 ${APP}，请先运行 ./02-build.sh" >&2
    exit 1
fi

mkdir -p "${RES}"
for d in apps color-palettes color-schemes cursor-themes emoji graphics html icons keymaps themes wallpapers; do
    if [[ -d "${ROOT}/Base/res/${d}" ]]; then
        ln -sfn "${ROOT}/Base/res/${d}" "${RES}/${d}"
    fi
done
if [[ -d "${ROOT}/Base/res/fonts" ]]; then
    mkdir -p "${RES}/fonts"
    cp -r "${ROOT}/Base/res/fonts/." "${RES}/fonts/" 2>/dev/null || true
fi

if [[ ! -e /res ]]; then
    ln -s "${RES}" /res
fi
[[ -f /etc/WindowServer.ini ]] || touch /etc/WindowServer.ini

echo "== 启动 WindowServer =="
"${WS}" "$@" &
WS_PID=$!

cleanup() {
    kill "${WS_PID}" 2>/dev/null || true
    wait "${WS_PID}" 2>/dev/null || true
}
trap cleanup EXIT

# 等 WindowServer 把 socket 建起来
for _ in $(seq 1 100); do
    [[ -S /tmp/portal/window ]] && break
    sleep 0.05
done

if [[ ! -S /tmp/portal/window ]]; then
    echo "WindowServer 没有创建 /tmp/portal/window" >&2
    exit 1
fi

echo "== 启动 AnalogClock =="
"${APP}"
