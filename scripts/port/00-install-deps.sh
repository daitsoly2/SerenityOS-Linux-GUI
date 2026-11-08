#!/usr/bin/env bash
# 安装 Debian 13 (trixie) 上的构建依赖。
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "请以 root 运行：sudo $0" >&2
    exit 1
fi

apt-get update
# 核心工具链 + Lagom(GUI/合成器) 所需的宿主开发库。
# 说明：旧清单只有 SDL2/GL，但当前合成器走 DRM/KMS + libinput + Wayland，
# 下列 libdrm/libgbm/libinput/libwayland/dbus/xkbcommon 等是 CMake
# `pkg-config` 阶段硬性查找的，缺任何一个都会在 configure 直接失败。
apt-get install -y --no-install-recommends \
    build-essential clang lld cmake ninja-build pkg-config python3 \
    curl ca-certificates unzip tar xz-utils \
    libsdl2-dev libgl1-mesa-dev libegl1-mesa-dev libgles2-mesa-dev \
    libdrm-dev libgbm-dev libinput-dev libudev-dev \
    libx11-dev libxcomposite-dev libxcursor-dev libxi-dev libxtst-dev \
    libdbus-1-dev libsystemd-dev libwayland-dev libxkbcommon-dev \
    libpixman-1-dev libasound2-dev \
    libxcb-imdkit-dev libxcb-imdkit1 \
    xvfb

# libxcb-imdkit: serenity-wlroots-host 的 XIM 前端（XimServer）链接它。
# 缺这个运行时库时桥会以 status 127 退出，七次后 serenitid 放弃重启，
# 表现为「所有 Wayland/X11 Linux 应用都打不开」——很容易误判成应用本身坏了。

# 可选组件（不参与主构建，按需安装）：
#   输入法   : sudo apt install librime-dev   (serenity-engine 直链 librime)
#   打印     : sudo apt install cups cups-client
#   内核模块 : sudo apt install linux-headers-$(uname -r)   (KernelModules/serenity_compat)
#   浏览器   : sudo apt install firefox-esr

echo
echo "依赖安装完成。"
echo "下一步：sudo ./01-fetch-caches.sh"
