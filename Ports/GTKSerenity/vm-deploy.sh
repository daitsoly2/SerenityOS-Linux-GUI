#!/usr/bin/env bash
# Deploy build/stage into the VM at /serenity/gtk and wire ldconfig.
# The VM's desktop already uses DISPLAY=:100 (Xwayland), and
# /etc/ld.so.conf.d/30-serenity-gtk.conf (written here) makes the private
# prefix win over /usr/lib for every GTK/glib soname.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAGE="$HERE/build/stage"
SSH="./vm-ssh.sh"
[[ -d "$STAGE/lib" ]] || { echo "error: build first (./master-build.sh)" >&2; exit 1; }

echo "== rsync stage -> VM:/tmp/serenity-gtk-stage =="
rsync -az --delete "$STAGE/" -e "$SSH" serenity@127.0.0.1:/tmp/serenity-gtk-stage/

echo "== rsync lagom libs (for libserenitypaint bridge) -> VM =="
"$SSH" serenity@127.0.0.1 'mkdir -p /tmp/serenity-lagom-libs'
rsync -az "$HERE/../../Build/lagom/lib/"/liblagom-*.so* -e "$SSH" serenity@127.0.0.1:/tmp/serenity-lagom-libs/

echo "== rsync GLOBAL theme -> VM =="
"$SSH" serenity@127.0.0.1 'mkdir -p /tmp/serenity-gtk-theme'
rsync -az --delete "$HERE/theme/" -e "$SSH" serenity@127.0.0.1:/tmp/serenity-gtk-theme/

echo "== install on VM =="
"$SSH" serenity@127.0.0.1 'sudo -n bash -s' <<'EOF'
set -e
PREFIX=/serenity/gtk
install -d "$PREFIX/lib" "$PREFIX/libexec" "$PREFIX/share" "$PREFIX/res/themes" "$PREFIX/etc"
cp -a /tmp/serenity-gtk-stage/lib/. "$PREFIX/lib/"
# Lagom LibGfx + deps for the libserenitypaint bridge (real ClassicStylePainter).
cp -a /tmp/serenity-lagom-libs/. "$PREFIX/lib/" 2>/dev/null || true
cp -a /tmp/serenity-gtk-stage/share/. "$PREFIX/share/" 2>/dev/null || true
# themes from the running Serenity system (already has every .ini)
if [ -d /serenity/res/themes ]; then
  cp -a /serenity/res/themes/. "$PREFIX/res/themes/" 2>/dev/null || true
fi
if [ -f /etc/WindowServer.ini ] && [ ! -f "$PREFIX/etc/WindowServer.ini" ]; then
  cp -a /etc/WindowServer.ini "$PREFIX/etc/" 2>/dev/null || true
fi
# System-wide theme: one copy under /usr/share/themes/Serenity, used by every
# GTK3/GTK4/libadwaita app via GTK_THEME=Serenity.  install.sh also removes the
# stale ~/.config/gtk-4.0/gtk.css Win2000 override (USER priority 800, which
# would otherwise beat both the theme and the shim stylesheet).
if [ -f /tmp/serenity-gtk-theme/install.sh ]; then
  sh /tmp/serenity-gtk-theme/install.sh /
fi
printf 'SERENITY_PREFIX=%s\n' "$PREFIX" > /etc/environment.d/50-serenity-gtk.conf
printf '%s/lib\n%s/lib/x86_64-linux-gnu\n' "$PREFIX" "$PREFIX" > /etc/ld.so.conf.d/30-serenity-gtk.conf
ldconfig
echo "ldconfig done"
ldconfig -p | grep -E 'libgtk-4\.so\.1|libadwaita-1\.so\.0|libglib-2\.0\.so\.0' | head
EOF
echo "== deploy complete =="
