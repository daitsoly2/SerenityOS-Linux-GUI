#!/usr/bin/env bash
# Prove the fork did not change the public ABI.
#
#   ./check-abi.sh build/stage
#
# Hard requirements:
#   * SONAME unchanged
#   * no exported symbol removed
#   * none of our new internal symbols leaked into .dynsym
# Version-to-version upstream differences are reported separately and do not
# fail the check.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAGE="${1:-$HERE/build/stage}"
MULTIARCH="$(gcc -dumpmachine)"
WORK="$HERE/build/abi-check"
mkdir -p "$WORK"

find_ref() {
    local soname="$1" f
    for f in $(ldconfig -p 2>/dev/null | awk -v s="$soname" '$1==s {print $NF}'); do
        [[ -f "$f" ]] && { echo "$f"; return 0; }
    done
    for f in "/usr/lib/$MULTIARCH/$soname" "/usr/lib/x86_64-linux-gnu/$soname" \
             "/lib/x86_64-linux-gnu/$soname"; do
        [[ -f "$f" ]] && { echo "$f"; return 0; }
    done
    return 1
}

compare() {
    local label="$1" soname="$2" new="$3" ref
    echo "--- $label ($soname)"
    if [[ ! -f "$new" ]]; then echo "    SKIP: built library not found: $new"; return 0; fi
    if ! ref="$(find_ref "$soname")"; then echo "    SKIP: reference system library not found"; return 0; fi
    echo "    ref: $ref"
    echo "    new: $new"

    local new_soname ref_soname
    new_soname="$(readelf -d "$new" | sed -n 's/.*SONAME.*\[\(.*\)\]/\1/p')"
    ref_soname="$(readelf -d "$ref" | sed -n 's/.*SONAME.*\[\(.*\)\]/\1/p')"
    if [[ "$new_soname" != "$ref_soname" ]]; then
        echo "    FAIL: SONAME changed: '$ref_soname' -> '$new_soname'"; return 1
    fi
    echo "    SONAME: $new_soname (unchanged)"

    nm -D --defined-only "$ref" 2>/dev/null | awk '{print $NF}' | sort -u > "$WORK/$label.ref"
    nm -D --defined-only "$new" 2>/dev/null | awk '{print $NF}' | sort -u > "$WORK/$label.new"

    local removed leaked
    removed="$(comm -23 "$WORK/$label.ref" "$WORK/$label.new" | wc -l)"
    leaked="$(grep -c -E '^(serenity_|_gtk_serenity_|adw_serenity_)' "$WORK/$label.new" || true)"
    echo "    exported symbols: ref=$(wc -l < "$WORK/$label.ref") new=$(wc -l < "$WORK/$label.new")"
    echo "    removed: $removed   leaked internal symbols: $leaked"
    if [[ "$removed" -gt 0 ]]; then
        echo "    FAIL: exported symbols were removed:"
        comm -23 "$WORK/$label.ref" "$WORK/$label.new" | head -10 | sed 's/^/      /'
        return 1
    fi
    if [[ "$leaked" -gt 0 ]]; then
        echo "    FAIL: internal Serenity symbols are exported:"
        grep -E '^(serenity_|_gtk_serenity_|adw_serenity_)' "$WORK/$label.new" | head -10 | sed 's/^/      /'
        return 1
    fi
    echo "    OK"
}

rc=0
compare gtk3       libgtk-3.so.0  "$STAGE/lib/$MULTIARCH/libgtk-3.so.0"       || rc=1
compare gtk3       libgtk-3.so.0  "$STAGE/lib/x86_64-linux-gnu/libgtk-3.so.0" || true
compare gtk4       libgtk-4.so.1  "$STAGE/lib/$MULTIARCH/libgtk-4.so.1"       || rc=1
compare gtk4       libgtk-4.so.1  "$STAGE/lib/x86_64-linux-gnu/libgtk-4.so.1" || true
compare libadwaita libadwaita-1.so.0 "$STAGE/lib/$MULTIARCH/libadwaita-1.so.0" || rc=1

if [[ "$rc" -ne 0 ]]; then
    echo "ABI check FAILED" >&2
    exit 1
fi
echo "ABI check passed."
