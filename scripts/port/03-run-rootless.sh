#!/usr/bin/env bash
# 无 root 运行 WindowServer。
#
#   ./03-run-rootless.sh                      # headless，写 Build/screenshot.ppm
#   ./03-run-rootless.sh --width 1600 --height 900
#   SERENITY_KMSDRM=1 ./03-run-rootless.sh    # 有 /dev/dri 时直接走 KMSDRM
#   ./03-run-rootless.sh --screenshot-delay 3000
#
# 与 03-run.sh 的区别：不需要 sudo。
#
# Serenity 的 Gfx/WindowServer 代码里有多处硬编码的 "/res"（主题、光标、
# 字体、emoji），没法全部靠环境变量绕开。这里用 unprivileged user namespace
# 造一个只读沙箱根：里面 /res 指向 Build/lagom/Root/res，/etc 是私有的可写
# 目录（WindowServer 要写 /etc/WindowServer.ini），/usr、$ROOT、/tmp 只做
# bind mount。宿主文件系统不会被改动。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${ROOT}/Build/lagom"
BIN="${BUILD}/bin/serenity-window-server"
RES="${BUILD}/Root/res"
RUNROOT="${BUILD}/runroot"

if [[ ! -x "${BIN}" ]]; then
    echo "找不到 ${BIN}，请先运行 ./02-build.sh" >&2
    exit 1
fi

# --- 1. 资源树（和 03-run.sh 一致） -------------------------------------
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

# 已经有可用的 /res（例如之前 sudo 跑过 03-run.sh）就不必折腾命名空间。
if [[ -r /res/themes/Default.ini && -w /etc ]]; then
    echo "/res 已可用，直接在当前环境运行"
    exec "${BIN}" "$@"
fi

# --- 2. 沙箱根 ---------------------------------------------------------
mkdir -p "${RUNROOT}"/{etc,tmp,usr,proc}
ln -sfn usr/lib "${RUNROOT}/lib"
ln -sfn usr/lib64 "${RUNROOT}/lib64"
ln -sfn usr/bin "${RUNROOT}/bin"
ln -sfn usr/sbin "${RUNROOT}/sbin"
ln -sfn "${RES}" "${RUNROOT}/res"

# chroot 里自己的 /etc：拷最小的几条，再补一个空的 WindowServer.ini。
for f in ld.so.cache ld.so.conf nsswitch.conf passwd group hosts resolv.conf localtime; do
    [[ -f "/etc/${f}" ]] && cp "/etc/${f}" "${RUNROOT}/etc/" 2>/dev/null || true
done
[[ -d /etc/ld.so.conf.d ]] && cp -r /etc/ld.so.conf.d "${RUNROOT}/etc/" 2>/dev/null || true
[[ -f "${RUNROOT}/etc/WindowServer.ini" ]] || : > "${RUNROOT}/etc/WindowServer.ini"

# $ROOT 在沙箱内的挂载点（保持绝对路径一致，二进制和 Root/res 才能找到）。
mkdir -p "${RUNROOT}${ROOT}"

# --- 3. 显示后端 / 截图参数 ---------------------------------------------
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-}"
export LD_LIBRARY_PATH="${BUILD}/lib:/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

ARGS=("$@")
if [[ -n "${SERENITY_KMSDRM:-}" ]]; then
    echo "使用 SDL kmsdrm 后端"
    export SDL_VIDEODRIVER=kmsdrm
elif [[ -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; then
    export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-}"
elif [[ -z "${SDL_VIDEODRIVER:-}" ]]; then
    echo "未检测到 DISPLAY / WAYLAND_DISPLAY，使用 headless 截图模式"
    export SDL_VIDEODRIVER=dummy
    OUT="${ROOT}/Build/screenshot.ppm"
    has_screenshot=0
    for a in "${ARGS[@]:-}"; do [[ "$a" == "--screenshot" ]] && has_screenshot=1; done
    if [[ ${has_screenshot} -eq 0 ]]; then
        ARGS=(--screenshot "${OUT}" --screenshot-delay 1500 "${ARGS[@]:-}")
    fi
fi

# --- 4. 进命名空间并 chroot ---------------------------------------------
# 注意：/proc、/dev、/dev/null 在 unprivileged userns 里无法 bind，也没有
# 权限 mknod，所以沙箱里没有这两个目录。WindowServer 本身不需要它们。
SANDBOX='
set -e
RR="$1"; ROOTSRC="$2"; shift 2
mount --bind /usr "${RR}/usr"
mount --bind "${ROOTSRC}" "${RR}${ROOTSRC}"
mount --bind /tmp "${RR}/tmp"
cd /
exec chroot "${RR}" "${BIN}" "$@"
'
export BIN
exec unshare -r -m --propagation private bash -c "${SANDBOX}" -- "${RUNROOT}" "${ROOT}" "${BIN}" "${ARGS[@]}"
