#!/usr/bin/env bash
# Switch the whole desktop between the Vista theme and the classic look.
#
#   sudo scripts/port/serenity-theme.sh vista     # Aero / glass on
#   sudo scripts/port/serenity-theme.sh classic  # theme "off" -> Win2000
#   scripts/port/serenity-theme.sh status        # show current selection
#
# "Closing the theme" is what XP/Vista did when the Themes service was stopped:
# the whole shell falls back to the classic widgets. Here that is a single
# [Theme] Name switch in /etc/WindowServer.ini, plus regenerating the GTK/Qt
# themes from the very same ini so Linux apps follow along. See VISTA_THEME.md.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONFIG="/etc/WindowServer.ini"
THEME_DIR="${SERENITY_THEME_DIR:-/res/themes}"
GEN="${ROOT}/scripts/port/serenity-theme-gen.py"

VISTA_THEME="Redmond Vista"
CLASSIC_THEME="Redmond 2000"

usage() {
    echo "usage: $0 {vista|classic|status} [--no-restart]" >&2
    exit 2
}

current_theme() {
    python3 - "$CONFIG" <<'PY'
import configparser, sys
c = configparser.ConfigParser()
c.optionxform = str
try:
    c.read(sys.argv[1])
    print(c.get("Theme", "Name", fallback="Default"))
except Exception:
    print("Default")
PY
}

set_theme() {
    local name="$1" config="$2"
    python3 - "$config" "$name" <<'PY'
import configparser, sys
path, name = sys.argv[1], sys.argv[2]
c = configparser.ConfigParser()
c.optionxform = str
with open(path, encoding="utf-8") as f:
    c.read_file(f)
if not c.has_section("Theme"):
    c.add_section("Theme")
c.set("Theme", "Name", name)
with open(path, "w", encoding="utf-8") as f:
    c.write(f, space_around_delimiters=False)
PY
}

command="${1:-}"
[[ -n "$command" ]] || usage
shift || true
NO_RESTART=0
[[ "${1:-}" == "--no-restart" ]] && NO_RESTART=1

case "$command" in
    status)
        echo "Theme: $(current_theme)"
        exit 0
        ;;
    vista)  TARGET="$VISTA_THEME" ;;
    classic) TARGET="$CLASSIC_THEME" ;;
    *) usage ;;
esac

if [[ ! -f "${THEME_DIR}/${TARGET}.ini" ]]; then
    # Fall back to the source tree (useful when running from the repo before
    # the theme has been installed into /res).
    if [[ -f "${ROOT}/Base/res/themes/${TARGET}.ini" ]]; then
        THEME_DIR="${ROOT}/Base/res/themes"
    else
        echo "error: theme not found: ${THEME_DIR}/${TARGET}.ini" >&2
        exit 1
    fi
fi

if [[ ! -w "$CONFIG" ]]; then
    echo "error: $CONFIG is not writable; run with sudo." >&2
    exit 1
fi

echo "== setting system theme: $TARGET =="
set_theme "$TARGET" "$CONFIG"

echo "== regenerating GTK3 / GTK4 / libadwaita / Qt themes =="
python3 "$GEN" --theme "${THEME_DIR}/${TARGET}.ini" --out /usr >/dev/null
echo "   $(current_theme)"

if [[ "$NO_RESTART" -eq 0 ]]; then
    # `list-unit-files | grep '^name'` is unreliable: the unit may be a
    # drop-in/static unit or the output may be column-padded, and the theme
    # then only changes in the ini while WindowServer keeps the old palette.
    # `systemctl cat` exits 0 for any unit systemd can resolve.
    if systemctl cat serenity-desktop.service >/dev/null 2>&1; then
        echo "== restarting desktop session =="
        systemctl restart serenity-desktop.service
    else
        echo "== no serenity-desktop.service; restart the session manually ==" >&2
    fi
fi

echo "done. 'theme off' == '$0 classic'."
