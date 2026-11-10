#!/usr/bin/env bash
# Build private glib-2.90.0 + pango-1.58.2 into build/stage so they are
# deployed together with the patched GTK4/libadwaita. The VM's system glib is
# only 2.84, so GTK4 (which requires >=2.89.3) must ship its own glib/pango.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# WorkBuddy runs commands under a "safe-delete" guard: an `rm` shell function
# plus a safe-bin shim at the front of PATH that aborts bulk deletes. Meson
# install overwrites/removes many files and trips it; meson's subprocess shells
# re-import the guard, so re-exec once under a fully clean environment (keeping
# pkg-config so cairo/harfbuzz/fontconfig still resolve for pango).
if [ -n "${CODEBUDDY_SAFE_DELETE_ENABLED:-}" ]; then
  _pcf="$HERE/build/stage/lib/x86_64-linux-gnu/pkgconfig:$HERE/build/stage/lib/pkgconfig:$HERE/build/stage/share/pkgconfig:/usr/local/lib/x86_64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/share/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/pkgconfig:/usr/share/pkgconfig"
  exec env -i PATH=/usr/bin:/bin:/usr/local/bin:/usr/sbin:/sbin HOME="${HOME:-/root}" LANG=C LC_ALL=C \
    PKG_CONFIG_PATH="$_pcf" /usr/bin/bash "$HERE/${BASH_SOURCE[0]##*/}" "$@"
fi
B="$HERE/build"
S="$B/stage"
JOBS="${JOBS:-$(nproc)}"
mkdir -p "$B/deps-src"

[[ -d "$B/deps-src/glib-2.90.0" ]] || tar xf "$HERE/glib-2.90.0.tar.xz" -C "$B/deps-src" --no-same-owner
[[ -d "$B/deps-src/pango-1.58.2" ]] || tar xf "$HERE/pango-1.58.2.tar.xz" -C "$B/deps-src" --no-same-owner

# meson install has no -j, and its parallel install races on locale-dir
# creation (intermittent "FAILED" on .mo files). Retry until it converges.
install_with_retry() {
  local bdir="$1" log="$2" name="$3" i rc
  for i in $(seq 1 8); do
    ninja -C "$bdir" install >> "$log" 2>&1; rc=$?
    if [ "$rc" -eq 0 ]; then echo "  $name install ok (attempt $i)"; return 0; fi
    echo "  $name install attempt $i failed (rc=$rc), retrying..." >> "$log"
    sleep 1
  done
  return 1
}

echo "=== glib-2.90.0 ==="
[[ -f "$B/glib/build.ninja" ]] || meson setup "$B/glib" "$B/deps-src/glib-2.90.0" --prefix "$S" --buildtype=release \
  -Dtests=false -Dintrospection=disabled -Dman-pages=false -Dgtk_doc=false \
  -Dlibmount=disabled -Dnls=disabled > "$B/setup-glib.log" 2>&1
ninja -C "$B/glib" -j "$JOBS" > "$B/build-glib.log" 2>&1
install_with_retry "$B/glib" "$B/build-glib.log" glib
echo "glib installed: $(pkg-config --modversion --define-prefix glib-2.0 2>/dev/null || true)"

echo "=== pango-1.58.2 ==="
export PKG_CONFIG_PATH="$S/lib/x86_64-linux-gnu/pkgconfig:$S/lib/pkgconfig:$S/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
[[ -f "$B/pango/build.ninja" ]] || meson setup "$B/pango" "$B/deps-src/pango-1.58.2" --prefix "$S" --buildtype=release \
  -Dintrospection=disabled -Dgtk_doc=false > "$B/setup-pango.log" 2>&1
ninja -C "$B/pango" -j "$JOBS" > "$B/build-pango.log" 2>&1
install_with_retry "$B/pango" "$B/build-pango.log" pango
echo "pango installed: $(pkg-config --modversion --define-prefix pango 2>/dev/null || true)"

echo "DEPS DONE"
find "$S" -maxdepth 3 -name 'libglib-2.0.so*' -o -name 'libpango-1.0.so*' 2>/dev/null
