#!/usr/bin/env bash
# Build libserenitypaint.so: a C-ABI bridge that calls Serenity's REAL
# Gfx::StylePainter (from the Lagom host build of LibGfx) instead of a cairo
# reimplementation. libgtk dlopen()s it, so libgtk stays pure-C.
#
# The palette comes from libserenity-ui (the same WindowServer.ini /
# <Theme>.ini parser the GTK style provider uses), so that file is compiled
# into the .so rather than duplicating the colours here.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
LAGOM="$ROOT/Build/lagom"
OUT="$HERE/build/stage/lib"
BR="$HERE/build/bridge"
mkdir -p "$OUT" "$BR"

echo "== compile serenity-ui (palette source) =="
gcc -std=c11 -O2 -fPIC -c "$HERE/serenity-ui/serenity-ui.c" \
    -D_GNU_SOURCE -fvisibility=hidden \
    -I"$HERE/serenity-ui" \
    -o "$BR/serenity-ui.o"

echo "== compile bridge =="
g++ -std=gnu++23 -O2 -fPIC -shared -fno-exceptions -fno-rtti -DBUILDING_LAGOM \
    -I"$ROOT/AK" -I"$ROOT/Userland/Libraries" -I"$ROOT" -I"$HERE/serenity-ui" \
    "$HERE/serenity-paint-bridge/serenity-paint-bridge.cpp" "$BR/serenity-ui.o" \
    -L"$LAGOM/lib" -Wl,-rpath,"$LAGOM/lib" \
    -llagom-gfx -llagom-core -llagom-ak -lpthread -lm \
    -o "$BR/libserenitypaint.so"

cp -f "$BR/libserenitypaint.so" "$OUT/"
echo "== built: $OUT/libserenitypaint.so =="
nm -D --defined-only "$OUT/libserenitypaint.so" | grep -E "serenity_paint_" || true
if ldd "$OUT/libserenitypaint.so" | grep -q "not found"; then
    ldd "$OUT/libserenitypaint.so" | grep "not found"
    echo "MISSING DEPS"
    exit 1
fi
echo "ok"
