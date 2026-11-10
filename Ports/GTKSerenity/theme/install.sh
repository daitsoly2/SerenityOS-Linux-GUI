#!/bin/sh
# Install the Serenity "painter" theme SYSTEM-WIDE.
#
#   ./install.sh                 # install into /usr/share/themes on this host
#   ./install.sh /tmp/stage      # install into a staging root (for VM deploy)
#
# This is a GLOBAL theme: one copy under /usr/share/themes/Serenity, seen by
# every GTK3 / GTK4 / libadwaita app through GTK_THEME=Serenity.  It replaces
# the old serenity-theme-gen.py Win2000 stylesheet, which drew bevels and grey
# popovers itself and fought the real LibGfx painter injected by the vendored
# GTK shims.
#
# The only per-user CSS GTK4 can load (~/.config/gtk-4.0/gtk.css) is a local
# override with USER priority (800) -- higher than the shim's APPLICATION+1
# (601) and higher than the theme itself.  Leaving the old Win2000 file there
# meant the "global" theme was silently overridden for every app.  This
# script removes it (after backing it up) so the system theme really is the
# single source of truth.  Use --keep-user-css to skip that step.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DEST=${1:-/}
KEEP_USER_CSS=0
for arg in "$@"; do
    [ "$arg" = "--keep-user-css" ] && KEEP_USER_CSS=1
done
# A staging root is passed as the first argument; --keep-user-css is never a
# path, so ignore it if it was the only argument.
[ "$DEST" = "--keep-user-css" ] && DEST=/

ROOT=${DEST%/}
THEME_DIR="$ROOT/usr/share/themes/Serenity"
install -d "$THEME_DIR/gtk-3.0" "$THEME_DIR/gtk-4.0"
install -m 644 "$HERE/index.theme"          "$THEME_DIR/index.theme"
install -m 644 "$HERE/gtk-3.0/gtk.css"      "$THEME_DIR/gtk-3.0/gtk.css"
install -m 644 "$HERE/gtk-4.0/gtk.css"      "$THEME_DIR/gtk-4.0/gtk.css"
# The old serenity-theme-gen.py used to drop a second Win2000 stylesheet
# here.  GTK4 never loads it, but leaving it behind makes the theme look like
# it still has two conflicting sources of truth.
if [ -f "$THEME_DIR/gtk-4.0/libadwaita.css" ]; then
    cp -p "$THEME_DIR/gtk-4.0/libadwaita.css" "$THEME_DIR/gtk-4.0/libadwaita.css.pre-global.bak"
    rm -f "$THEME_DIR/gtk-4.0/libadwaita.css"
    echo "removed stale Win2000 libadwaita.css (backed up)"
fi
echo "installed $THEME_DIR (index.theme + gtk-3.0 + gtk-4.0)"

if [ "$KEEP_USER_CSS" -eq 0 ] && [ "$DEST" = "/" ]; then
    for home in /home/* /root; do
        [ -d "$home" ] || continue
        css="$home/.config/gtk-4.0/gtk.css"
        if [ -f "$css" ]; then
            backup="$css.pre-serenity-global-$(date +%Y%m%d-%H%M%S).bak"
            cp -p "$css" "$backup"
            rm -f "$css"
            echo "removed local override $css (backup: $backup)"
        fi
    done
fi
