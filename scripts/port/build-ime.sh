#!/bin/sh
# Build and install the fcitx5 `serenityime` addon.
#
# The addon is the engine side of the input method: fcitx5 loads it, it listens
# on a unix socket, and LibGUI's InputMethod client talks to it (see
# Ports/LinuxGUI/IMEFrontend/README.md). It is built separately from the Lagom
# tree because it needs the fcitx5 core headers.
#
#   scripts/port/build-ime.sh                # build + install system-wide
#   scripts/port/build-ime.sh --build-only   # build into Build/ime only
#
# Build dependencies (Debian):
#   sudo apt install cmake ninja-build libfcitx5core-dev
# Runtime, to actually type Chinese:
#   sudo apt install fcitx5 fcitx5-chinese-addons
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SOURCE="${ROOT}/Ports/LinuxGUI/IMEFrontend"
BUILD="${ROOT}/Build/ime"

build_only=0
case "${1:-}" in
    --build-only) build_only=1 ;;
    "") ;;
    *) echo "usage: $0 [--build-only]" >&2; exit 2 ;;
esac

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake is missing" >&2
    exit 1
fi

echo "== configuring =="
cmake -S "${SOURCE}" -B "${BUILD}" -G Ninja -DCMAKE_BUILD_TYPE=Release
echo "== building =="
cmake --build "${BUILD}" -j"$(nproc 2>/dev/null || echo 4)"

if [ "${build_only}" = 1 ]; then
    echo "built: ${BUILD}/bin/libserenityime.so"
    exit 0
fi

# fcitx5 addons live in <libdir>/fcitx5; Debian uses the multiarch libdir, so
# look for the directory rather than guessing /usr/lib/fcitx5.
addon_dir="$(find /usr/lib /usr/local/lib -maxdepth 4 -type d -name fcitx5 2>/dev/null | head -n 1)"
if [ -z "${addon_dir}" ]; then
    echo "could not find an fcitx5 addon directory; is fcitx5 installed?" >&2
    echo "  sudo apt install fcitx5 fcitx5-chinese-addons" >&2
    exit 1
fi

echo "== installing into ${addon_dir} =="
install -m755 "${BUILD}/bin/libserenityime.so" "${addon_dir}/"
install -d /usr/share/fcitx5/addon
install -m644 "${SOURCE}/serenityime.conf" /usr/share/fcitx5/addon/

echo
echo "done. Reload fcitx5 so it picks up the addon:"
echo "  fcitx5 -r            # or log out and back in"
echo
echo "If Chinese still does not type, check that an engine is configured:"
echo "  fcitx5-configtool    # add Pinyin under Input Method"
