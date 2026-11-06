#!/usr/bin/env bash
# Build + stage the GTK3 "serenity" IM module.
#
# Usage (inside the Debian 13 VM, where libgtk-3-dev is installed):
#   ./build.sh [install-dir]        # default: /tmp/serenity-gtk-im
#
# Then run a GTK3 app with:
#   GTK_IM_MODULE=serenity \
#   GTK_IM_MODULE_FILE=<install-dir>/immodules.cache \
#   <gtk3-app>
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-/tmp/serenity-gtk-im}"
mkdir -p "$OUT"

echo "== compiling im-serenity.so =="
cc -shared -fPIC -O2 -Wall -Wno-deprecated-declarations \
    -o "$OUT/im-serenity.so" "$SRC_DIR/im-serenity.c" \
    $(pkg-config --cflags --libs gtk+-3.0)

echo "== generating immodules.cache =="
# Start from the system cache so all stock modules remain available, then
# append our module. GTK honours the first matching context id; our id
# ("serenity") is unique so order does not matter.
SYSTEM_CACHE="/usr/lib/x86_64-linux-gnu/gtk-3.0/3.0.0/immodules.cache"
{
    if [[ -f "$SYSTEM_CACHE" ]]; then
        grep -v '^#' "$SYSTEM_CACHE"
    fi
    printf '"%s/im-serenity.so" \n' "$OUT"
    printf '"serenity" "Serenity IME" "serenity" "/usr/share/locale" "zh:ja:ko:*" \n'
} > "$OUT/immodules.cache"

echo "== staged in $OUT =="
ls -la "$OUT"
echo
echo "Verify the module is loadable:"
echo "  GTK_IM_MODULE=serenity GTK_IM_MODULE_FILE=$OUT/immodules.cache \\"
echo "      GTK_DEBUG=modules gedit --new-window"
echo
echo "Debug log (when SERENITY_GTK_IM_DEBUG=1): /tmp/serenity-gtk-im.log"
