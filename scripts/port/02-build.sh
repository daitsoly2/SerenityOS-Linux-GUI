#!/usr/bin/env bash
# 配置并构建 Linux 版 WindowServer。
#
# 前置：00-install-deps.sh、01-fetch-caches.sh 已执行成功。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ROOT}/Build/lagom"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -d "${ROOT}/Build/caches/UCD" ]]; then
    echo "缺少 Build/caches，请先运行 ./01-fetch-caches.sh" >&2
    exit 1
fi

echo "== 配置 Lagom (${BUILD}) =="
cmake -S "${ROOT}/Meta/Lagom" -B "${BUILD}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DBUILD_LAGOM=ON \
    -DENABLE_LAGOM_LIBWEB=OFF \
    -DENABLE_LAGOM_LADYBIRD=OFF \
    -DENABLE_LAGOM_CCACHE=OFF \
    -DENABLE_JAKT=OFF \
    -DENABLE_NETWORK_DOWNLOADS=OFF \
    -DENABLE_ADOBE_ICC_PROFILES_DOWNLOAD=OFF \
    -DENABLE_COMMONMARK_SPEC_DOWNLOAD=OFF \
    -DENABLE_LINUX_GUI_PORT=ON \
    -DENABLE_LAGOM_GUI_APPS=ON

# 干净构建时，WindowServer 的 .o 可能先于 IPC 头生成，导致
# "WindowServer/WindowManagerClientEndpoint.h: file not found"。
# 先单独把头生成出来，再并行编译。
echo "== 预生成 IPC 头 =="
cmake --build "${BUILD}" --target \
    generate_WindowServerEndpoint.h \
    generate_WindowClientEndpoint.h \
    generate_WindowManagerServerEndpoint.h \
    generate_WindowManagerClientEndpoint.h \
    generate_ClipboardClientEndpoint.h \
    generate_ClipboardServerEndpoint.h \
    generate_NotificationClientEndpoint.h \
    generate_NotificationServerEndpoint.h \
    -j"${JOBS}"

echo "== 构建 WindowServer + AnalogClock (-j${JOBS}) =="
cmake --build "${BUILD}" --target serenity-window-server serenity-analog-clock -j"${JOBS}"

echo
echo "构建完成："
echo "  ${BUILD}/bin/serenity-window-server"
echo "  ${BUILD}/bin/serenity-analog-clock"
echo "下一步：sudo ./03-run.sh   （只开桌面）"
echo "        sudo ./04-run-app.sh（桌面 + 时钟 app）"
