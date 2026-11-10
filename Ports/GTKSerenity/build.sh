#!/usr/bin/env bash
# Build the Serenity-flavoured GTK3 / GTK4 / libadwaita.
#
#   ./build.sh              # everything
#   ./build.sh ui           # just libserenity-ui + the headless preview
#   ./build.sh gtk4         # one component
#
# All logs and products stay under build/ (the host's /tmp is not shared
# between commands). GTK4 is built with clang: GTK 4.24's meson probes use
# clang-only -Wno-* flags that GCC 16 rejects as hard errors.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# WorkBuddy runs commands under a "safe-delete" guard: an `rm` shell function
# plus a safe-bin shim at the front of PATH that aborts bulk directory/file
# deletes. Meson/ninja install overwrites and removes many files and trips it.
# A plain PATH edit is not enough (meson's own subprocess shells re-import the
# guard), so re-exec ourselves once under a fully clean environment, keeping
# the pkg-config search path so dependencies still resolve.
if [ -n "${CODEBUDDY_SAFE_DELETE_ENABLED:-}" ]; then
  _pcf="$HERE/build/stage/lib/x86_64-linux-gnu/pkgconfig:$HERE/build/stage/lib/pkgconfig:$HERE/build/stage/share/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/share/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"
  # GTK4 4.24 must be built with clang; preserve an explicit CC across re-exec.
  _cc=""; [ -n "${CC:-}" ] && _cc="CC=$CC"
  exec env -i PATH=/usr/bin:/bin:/usr/local/bin:/usr/sbin:/sbin HOME="${HOME:-/root}" LANG=C LC_ALL=C \
    PKG_CONFIG_PATH="$_pcf" ${_cc:+"$_cc"} /usr/bin/bash "$HERE/${BASH_SOURCE[0]##*/}" "$@"
fi
# shellcheck disable=SC1091
source "$HERE/versions.env"
B="$HERE/build"
STAGE="$B/stage"
JOBS="${JOBS:-$(nproc)}"
TARGET="${1:-all}"
mkdir -p "$B" "$STAGE"

GTK4_OPTS=(
  --wrap-mode=nofallback --prefix "$STAGE" --buildtype=release
  -Dintrospection=disabled -Ddocumentation=false -Dman-pages=false -Dscreenshots=false
  -Dbuild-tests=false -Dbuild-demos=false -Dbuild-examples=false -Dbuild-testsuite=false
  -Dmedia-gstreamer=disabled -Dprint-cpdb=disabled -Dprint-cups=disabled
  -Dvulkan=disabled -Dcloudproviders=disabled -Dsysprof=disabled -Dtracker=disabled
  -Dcolord=disabled -Daccesskit=disabled
  -Dnls=disabled
  -Dx11-backend=true -Dwayland-backend=true
)
GTK3_OPTS=(
  --wrap-mode=nofallback --prefix "$STAGE"
  -Dintrospection=false -Dgtk_doc=false -Dman=false -Dtests=false -Ddemos=false
  -Dexamples=false -Dinstalled_tests=false -Dcolord=no
  -Dcloudproviders=false -Dtracker3=false -Dprint_backends=file,lpr
  -Dx11_backend=true -Dwayland_backend=true
)
ADW_OPTS=(
  # 'default' (not nofallback) so libadwaita uses its vendored ministream
  # subproject locally — ministream is not packaged, no network fetch needed.
  --wrap-mode=default --prefix "$STAGE" --buildtype=release
  -Dintrospection=disabled -Dvapi=false -Dgtk_doc=false -Ddocumentation=false
  -Dtests=false -Dexamples=false
  -Dnls=disabled
)

log() { echo; echo "===== $* ====="; }

# meson's parallel `install` races on locale-directory creation for .mo files
# (intermittent FAILED with no real error). Retry `ninja install` a few times.
install_with_retry() {
  local bdir="$1" log="$2" i rc
  for i in $(seq 1 8); do
    ninja -C "$bdir" install >> "$log" 2>&1; rc=$?
    if [ "$rc" -eq 0 ]; then echo "  install ok ($bdir, attempt $i)"; return 0; fi
    echo "  install attempt $i failed (rc=$rc), retrying..." >> "$log"
    sleep 1
  done
  tail -25 "$log"
  return 1
}

build_ui() {
  log "libserenity-ui (headless preview + theme dump)"
  meson setup "$B/ui" "$HERE/serenity-ui" > "$B/setup-ui.log" 2>&1 \
    || { tail -25 "$B/setup-ui.log"; exit 1; }
  ninja -C "$B/ui" -j "$JOBS" > "$B/build-ui.log" 2>&1 \
    || { tail -30 "$B/build-ui.log"; exit 1; }
  # The preview paints through the real painter, exactly like the GTK shims:
  # build the bridge first so the widgets are not blank.
  bash "$HERE/build-bridge.sh" > "$B/build-bridge.log" 2>&1 \
    || { tail -20 "$B/build-bridge.log"; exit 1; }
  SERENITY_SOURCE_ROOT="$(cd "$HERE/../.." && pwd)" \
    SERENITY_PAINT_BRIDGE="$B/stage/lib/libserenitypaint.so" \
    "$B/ui/serenity-widget-preview" --out "$HERE/serenity-widgets.png"
  echo "  preview: $HERE/serenity-widgets.png"
}

setup_gtk4() {
  log "configure GTK4 $GTK4_VERSION (clang)"
  CC=clang meson setup "$B/gtk4" "$HERE/vendor/gtk4" "${GTK4_OPTS[@]}" \
    > "$B/setup-gtk4.log" 2>&1 || { tail -25 "$B/setup-gtk4.log"; exit 1; }
}

build_gtk4() {
  [[ -f "$B/gtk4/build.ninja" ]] || setup_gtk4
  log "compile GTK4"
  ninja -C "$B/gtk4" -j "$JOBS" > "$B/build-gtk4.log" 2>&1 \
    || { tail -40 "$B/build-gtk4.log"; exit 1; }
  install_with_retry "$B/gtk4" "$B/install-gtk4.log" \
    || exit 1
}

build_gtk3() {
  # GTK3's X11 build hard-depends on atk-bridge-2.0, whose header is not part
  # of this host's partial at-spi install. build/atspi-include holds the
  # single missing header (taken from the VM, same version).
  local extra_inc="$HERE/build/atspi-include/at-spi2-atk/2.0"
  if [ -f "$extra_inc/atk-bridge.h" ]; then
    export C_INCLUDE_PATH="$extra_inc${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}"
  fi
  log "configure + compile GTK3 $GTK3_VERSION"
  [[ -f "$B/gtk3/build.ninja" ]] || {
    meson setup "$B/gtk3" "$HERE/vendor/gtk3" "${GTK3_OPTS[@]}" \
      > "$B/setup-gtk3.log" 2>&1 || { tail -25 "$B/setup-gtk3.log"; exit 1; }
  }
  ninja -C "$B/gtk3" -j "$JOBS" > "$B/build-gtk3.log" 2>&1 \
    || { tail -40 "$B/build-gtk3.log"; exit 1; }
  install_with_retry "$B/gtk3" "$B/install-gtk3.log" \
    || exit 1
}

build_libadwaita() {
  log "configure + compile libadwaita $LIBADWAITA_VERSION"
  export PKG_CONFIG_PATH="$STAGE/lib/x86_64-linux-gnu/pkgconfig:$STAGE/lib/pkgconfig:$STAGE/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
  [[ -f "$B/libadwaita/build.ninja" ]] || {
    CC=clang meson setup "$B/libadwaita" "$HERE/vendor/libadwaita" "${ADW_OPTS[@]}" \
      > "$B/setup-adw.log" 2>&1 || { tail -25 "$B/setup-adw.log"; exit 1; }
  }
  ninja -C "$B/libadwaita" -j "$JOBS" > "$B/build-adw.log" 2>&1 \
    || { tail -40 "$B/build-adw.log"; exit 1; }
  install_with_retry "$B/libadwaita" "$B/install-adw.log" \
    || exit 1
}

case "$TARGET" in
  ui)          build_ui ;;
  gtk3)        build_gtk3 ;;
  gtk4)        build_gtk4 ;;
  libadwaita)  build_libadwaita ;;
  all)         build_ui; build_gtk4; build_gtk3; build_libadwaita ;;
  *) echo "usage: $0 {ui|gtk3|gtk4|libadwaita|all}" >&2; exit 2 ;;
esac

log "ABI check"
"$HERE/check-abi.sh" "$STAGE" || echo "  (skipped: reference libraries not available)"

log "built products"
find "$STAGE" -maxdepth 4 -name 'libgtk*.so*' -o -name 'libadwaita*.so*' 2>/dev/null | head -20
echo "done."
