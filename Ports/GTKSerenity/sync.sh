#!/usr/bin/env bash
# Re-sync the three upstream source trees and re-apply the Serenity fork.
#
#   ./sync.sh              # download (with sha256 check), unpack, patch
#   ./sync.sh --offline    # reuse tarballs already in vendor/, just unpack+patch
#
# Run this after changing versions.env. It is intentionally destructive to
# vendor/ only; our changes live in overlay/, serenity-ui/ and
# apply-serenity.py, never in the vendored tree.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$HERE/versions.env"
V="$HERE/vendor"
OFFLINE=0
[[ "${1:-}" == "--offline" ]] && OFFLINE=1

mkdir -p "$V"
cd "$V"

fetch() {
    local url="$1" sha="$2" file="$3"
    if [[ -f "$file" ]] && echo "$sha  $file" | sha256sum -c --status; then
        echo "  cached: $file"
        return
    fi
    if [[ "$OFFLINE" -eq 1 ]]; then
        echo "error: --offline but $file is missing or has a bad checksum" >&2
        exit 1
    fi
    echo "  downloading: $url"
    curl -fL --retry 3 --max-time 900 -o "$file.tmp" "$url"
    echo "$sha  $file.tmp" | sha256sum -c --status || {
        echo "error: checksum mismatch for $file" >&2; exit 1; }
    mv -f "$file.tmp" "$file"
}

echo "== syncing upstream (gtk3 $GTK3_VERSION / gtk4 $GTK4_VERSION / libadwaita $LIBADWAITA_VERSION) =="
fetch "$GTK3_URL"        "$GTK3_SHA256"        "$GTK3_ARCHIVE"
fetch "$GTK4_URL"        "$GTK4_SHA256"        "$GTK4_ARCHIVE"
fetch "$LIBADWAITA_URL"  "$LIBADWAITA_SHA256"  "$LIBADWAITA_ARCHIVE"

echo "== unpacking (--no-same-owner: build hosts may refuse chown) =="
rm -rf gtk3 gtk4 libadwaita gtk-bac4a949b151 gtk-4.24.1 libadwaita-1.10.0
tar --no-same-owner -xzf "$GTK3_ARCHIVE" 2>/dev/null || true
tar --no-same-owner -xJf "$GTK4_ARCHIVE" 2>/dev/null || true
tar --no-same-owner -xJf "$LIBADWAITA_ARCHIVE" 2>/dev/null || true

BR="$(find . -maxdepth 1 -type d -name 'gtk-*' ! -name 'gtk3' ! -name 'gtk4' | head -1)"
[[ -n "$BR" ]] || { echo "error: GTK3 branch directory not found" >&2; exit 1; }
mv "$BR" gtk3
mv gtk-4.24.1 gtk4
mv libadwaita-1.10.0 libadwaita

echo "== applying the Serenity fork =="
python3 "$HERE/apply-serenity.py"

echo "== done. versions: =="
for d in gtk3 gtk4 libadwaita; do
    printf '  %-12s ' "$d"
    grep -m1 -E "version ?:" "$d/meson.build" | tr -d ' ' || true
done
