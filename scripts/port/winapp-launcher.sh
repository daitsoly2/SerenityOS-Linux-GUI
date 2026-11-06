#!/bin/sh
# Windows app launcher for the Linux GUI port.
#
#   serenity-winapp [--game|--app|--mode <auto|app|game>] [--prefix <dir>] <exe> [args...]
#
# Two backends, picked automatically:
#
#   app  -> upstream (vanilla) wine                (system wine; DWProton fallback)
#   game -> <prefix>/bin/umu-run + <prefix>/opt/dwproton  (Steam Runtime + Proton +
#                                                  per-game fixes)
#
# "Is it a game?" is a heuristic, because an .exe does not say so. We look at the
# import table for the DLLs only games pull in (D3D/DXGI/XInput/Vulkan) and at
# engine markers next to the exe (UnityPlayer.dll, .pak, steam_api64.dll, ...).
# --game / --app / --mode / SERENITY_WINAPP_MODE override the guess. The
# per-program launchers written by WinApps.h pass --mode so a saved preference
# (auto/app/game) is honored exactly without re-guessing.
#
# Installed to <prefix>/bin/serenity-winapp by 05-install-prefix.sh.

set -e

prefix=${SERENITY_PREFIX:-/serenity}
export DISPLAY=${DISPLAY:-:100}
export WINEDEBUG=${WINEDEBUG:--all}
export WINEESYNC=${WINEESYNC:-1}
export WINEFSYNC=${WINEFSYNC:-1}

# Make the upstream wine reachable for the CJK-font repair step and the app
# branch even under a minimal systemd PATH.
PATH="/opt/wine-stable/bin:$PATH"
export PATH

# DWProton keeps its vkd3d runtime next to (not inside) the wine DLL dir, and a
# bare `wine` does not search it, so wined3d.dll / dxgi.dll fail to load with
# STATUS_DLL_NOT_FOUND (0xc0000135) and every app that imports dxgi dies before
# it maps a window. Only the DWProton/Proton runtime needs this; pointing an
# upstream wine at Proton's vkd3d DLLs breaks it (ABI/version mismatch), so it
# is applied per-branch instead of globally.
use_proton_dllpath() {
    WINEDLLPATH="$prefix/opt/dwproton/files/lib/vkd3d/x86_64-windows:$prefix/opt/dwproton/files/lib/vkd3d/i386-windows${WINEDLLPATH:+:$WINEDLLPATH}"
    export WINEDLLPATH
}

# Parse launcher flags; the .exe is the first non-flag argument.
mode=${SERENITY_WINAPP_MODE:-auto}
dry_run=0
wine_prefix_arg=""
while [ $# -gt 0 ]; do
    case "$1" in
        --game) mode=game; shift ;;
        --app) mode=app; shift ;;
        --mode) mode=$2; shift 2 ;;
        --prefix) wine_prefix_arg=$2; shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        --) shift; break ;;
        *) break ;;
    esac
done

# Per-program prefix (Ofuda-style isolation): each installed Windows program
# gets its own WINEPREFIX instead of one shared one. --prefix <dir> selects it;
# otherwise keep an inherited WINEPREFIX, or fall back to the session default
# (root session, so /root/.wine-dw - $HOME may be empty in a systemd unit).
if [ -n "$wine_prefix_arg" ]; then
    export WINEPREFIX="$wine_prefix_arg"
    mkdir -p "$WINEPREFIX"
elif [ -z "$WINEPREFIX" ]; then
    # /root is unreadable when the session runs as a normal user (e.g. serenity).
    export WINEPREFIX="${HOME:-/root}/.wine-dw"
fi

# Chinese Windows apps ask for SimSun / Microsoft YaHei. If the prefix has no
# CJK font registered under those names, GDI falls back to a Latin face and
# every character becomes a tofu box. Repair the prefix once, quietly; the
# marker file keeps this to a single test after the first run.
if [ -f "$WINEPREFIX/system.reg" ] && [ ! -f "$WINEPREFIX/.serenity-cjk-font" ]; then
    if [ -x "$prefix/bin/serenity-wine-fonts" ]; then
        "$prefix/bin/serenity-wine-fonts" --prefix "$WINEPREFIX" >/dev/null 2>&1 || true
    fi
fi

# Chinese Windows apps set WM_NAME with the UTF-8 atom; under LANG=C.UTF-8 Xlib
# reports an ASCII codeset, the conversion fails, and titles/text come out as
# garbage. Prefer a real UTF-8 locale when one is installed.
if locale -a 2>/dev/null | grep -qi '^zh_CN\.utf8$'; then
    export LANG=zh_CN.UTF-8
    export LC_ALL=zh_CN.UTF-8
fi

if [ $# -lt 1 ]; then
    echo "usage: serenity-winapp [--game|--app|--mode <auto|app|game>] [--prefix <dir>] <exe> [args...]" >&2
    exit 2
fi

target=$1
shift

# Validate --mode is one of the three understood values (auto/app/game).
case "$mode" in
    auto|app|game) ;;
    *) echo "serenity-winapp: invalid --mode '$mode' (expected auto|app|game)" >&2; exit 2 ;;
esac

# 游戏特征：图形/输入 DLL 的名字就在 PE 导入表里，或同目录有引擎/资源文件。
looks_like_game() {
    exe=$1
    if grep -aqiE 'd3d1[012]\.dll|dxgi\.dll|d3d9\.dll|d3d8\.dll|xinput[0-9_]*\.dll|dinput8\.dll|vulkan-1\.dll|openal32\.dll' "$exe" 2>/dev/null; then
        return 0
    fi

    dir=$(dirname "$exe")
    for marker in UnityPlayer.dll steam_api.dll steam_api64.dll d3d11.dll dxgi.dll d3d9.dll; do
        [ -f "$dir/$marker" ] && return 0
    done
    for pattern in "$dir"/*.pak "$dir"/*.uasset "$dir"/*.pck "$dir"/*.rpa "$dir"/*.assets; do
        [ -f "$pattern" ] && return 0
    done
    return 1
}

if [ "$mode" = auto ]; then
    if looks_like_game "$target"; then
        mode=game
    else
        mode=app
    fi
fi

if [ "$dry_run" = 1 ]; then
    echo "$target -> $mode"
    exit 0
fi

if [ "$mode" = game ] && [ -x "$prefix/bin/umu-run" ]; then
    # GAMEID 让 umu/protonfixes 找该游戏的修复脚本；不知道就 0（只走运行时）。
    # PROTONPATH 指向随端口发布的 DWProton，umu 据此用 dwproton 而非自带/默认 Proton。
    use_proton_dllpath
    GAMEID=${GAMEID:-0} UMU_ALLOW_ROOT=1 PROTONPATH="$prefix/opt/dwproton" \
        exec "$prefix/bin/umu-run" "$target" "$@"
fi

# app -> upstream (vanilla) wine. Proton is a gaming fork: it diverges from
# upstream, ships its own DLL overrides and expects the Steam Runtime. For
# office / utility / installer apps that is more surface to break, and running
# Proton's wine *without* its runtime (the old behaviour) is the worst of both
# worlds - a diverged wine with none of the game fixes. Prefer a real upstream
# wine; fall back to DWProton's wine only if none is installed.
if [ "$mode" = app ]; then
    vanilla_wine=$(command -v wine 2>/dev/null || true)
    for candidate in "$vanilla_wine" /opt/wine-stable/bin/wine /usr/local/bin/wine /usr/bin/wine; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            exec "$candidate" "$target" "$@"
        fi
    done
fi

# Fallback: DWProton's wine (game without umu-run, or app with no vanilla wine).
proton_wine="$prefix/opt/dwproton/files/bin/wine"
if [ -x "$proton_wine" ]; then
    use_proton_dllpath
    exec "$proton_wine" "$target" "$@"
fi

echo "serenity-winapp: neither a vanilla wine nor DWProton is installed" >&2
exit 127
