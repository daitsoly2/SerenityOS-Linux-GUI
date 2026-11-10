#!/usr/bin/env bash
# Install the patched GTK libraries on a Serenity Linux GUI host.
#
#   ./deploy.sh --private     # default: /serenity/gtk + ld.so.conf (reversible)
#   ./deploy.sh --replace     # overwrite the distro libraries (backs them up)
#   ./deploy.sh --vm          # push stage/ into the VM and run --private there
#   ./deploy.sh --status      # show which GTK libraries applications resolve
#
# Applications need no rebuild, no re-link and no environment variables:
# the SONAMEs are unchanged and the private prefix is picked up by ldconfig.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "$HERE/versions.env"
STAGE="$HERE/build/stage"
PREFIX="$SERENITY_GTK_PREFIX"
MODE="${1:---private}"
MULTIARCH="$(gcc -dumpmachine)"

VM_TOOLS="/mnt/user/serenity/Build/vm"
VM_SSH="$VM_TOOLS/vmssh.sh"
VM_SCP="$VM_TOOLS/vmscp.sh"

install_private() {
    echo "== private install into $PREFIX =="
    [[ -d "$STAGE/lib" ]] || { echo "error: build first (./build.sh)" >&2; exit 1; }
    install -d "$PREFIX/lib" "$PREFIX/libexec"
    cp -a "$STAGE/lib/." "$PREFIX/lib/"
    cp -a "$STAGE/share/." "$PREFIX/share/" 2>/dev/null || true

    # Theme source. serenity-ui reads $SERENITY_PREFIX/etc/WindowServer.ini
    # and $SERENITY_PREFIX/res/themes/<Name>.ini.
    local src_root
    src_root="$(cd "$HERE/../.." && pwd)"
    if [[ -d "$src_root/Base/res/themes" ]]; then
        install -d "$PREFIX/res/themes"
        cp -a "$src_root/Base/res/themes/." "$PREFIX/res/themes/"
    fi
    if [[ ! -f "$PREFIX/etc/WindowServer.ini" ]]; then
        install -d "$PREFIX/etc"
        [[ -f /etc/WindowServer.ini ]] && cp -a /etc/WindowServer.ini "$PREFIX/etc/" || true
    fi

    cat > /etc/environment.d/50-serenity-gtk.conf <<EOF
SERENITY_PREFIX=$PREFIX
SERENITY_SOURCE_ROOT=$src_root
EOF
    cat > /etc/ld.so.conf.d/30-serenity-gtk.conf <<EOF
$PREFIX/lib
$PREFIX/lib/$MULTIARCH
EOF
    ldconfig
    echo "   $(ldconfig -p | grep -m1 'libgtk-4.so.1' || true)"
}

install_replace() {
    echo "== replacing system GTK libraries =="
    local libdir="/usr/lib/$MULTIARCH" backup="/root/gtk-dist-backup-$(date +%Y%m%d-%H%M%S)"
    install -d "$backup"
    for so in libgtk-3.so.0 libgtk-4.so.1 libadwaita-1.so.0; do
        local built
        built="$(find "$STAGE" -name "$so" -type f | head -1)"
        [[ -n "$built" ]] || { echo "error: $so not built" >&2; exit 1; }
        if [[ -f "$libdir/$so" ]] && [[ ! -f "$backup/$so" ]]; then
            cp -a "$libdir/$so" "$backup/$so"
        fi
        install -m 644 "$built" "$libdir/$so"
        echo "   replaced $libdir/$so"
    done
    ldconfig
    echo "   backup: $backup"
}

status() {
    echo "== resolved GTK libraries =="
    ldconfig -p | grep -E 'libgtk-3\.so\.0|libgtk-4\.so\.1|libadwaita-1\.so\.0' || true
    echo "== theme =="
    if [[ -x "$HERE/serenity-ui/build/serenity-theme-dump" ]]; then
        "$HERE/serenity-ui/build/serenity-theme-dump" | head -6
    fi
    echo "== rollback switch =="
    echo "   SERENITY_GTK_LEGACY=1 restores stock Adwaita rendering at runtime"
}

vm_deploy() {
    echo "== deploying into VM =="
    [[ -x "$VM_SSH" && -x "$VM_SCP" ]] || {
        echo "error: VM helpers not found at $VM_TOOLS" >&2; exit 1; }
    "$VM_SSH" 'mkdir -p /tmp/serenity-gtk-stage /tmp/serenity-gtk'
    rsync -az --delete "$STAGE/" -e "ssh -p 2222 serenity@127.0.0.1" \
        serenity@127.0.0.1:/tmp/serenity-gtk-stage/ 2>/dev/null || {
        echo "note: rsync over ssh needs the session key; use $VM_SCP if it failed" >&2; }
    "$VM_SCP" "$HERE/deploy.sh" /tmp/serenity-gtk/deploy.sh
    "$VM_SCP" "$HERE/versions.env" /tmp/serenity-gtk/versions.env
    "$VM_SSH" 'cd /tmp/serenity-gtk && sudo -n bash deploy.sh --private'
}

case "$MODE" in
    --private) install_private ;;
    --replace) install_replace ;;
    --vm)      vm_deploy ;;
    --status)  status ;;
    *) echo "usage: $0 {--private|--replace|--vm|--status}" >&2; exit 2 ;;
esac

echo "== restarting desktop session =="
if systemctl cat serenity-desktop.service >/dev/null 2>&1; then
    systemctl restart serenity-desktop.service 2>/dev/null || true
fi
echo "done. Verify with: ./check-abi.sh $STAGE && ./deploy.sh --status"
