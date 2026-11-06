#!/bin/sh
# /bin/logout, /bin/shutdown and /bin/reboot for the Linux GUI port.
#
# Serenity's exit dialog (Userland/Services/Taskbar/ShutdownDialog.cpp) spawns
# those three Serenity paths. PortHostify maps them to serenity-logout /
# serenity-shutdown / serenity-reboot, so this one script is installed under
# all three names and dispatches on its own basename.
#
#   logout    SIGUSR1 to serenitid: it stops every child and re-execs itself,
#             so the login stage runs again. No privileges needed, because the
#             supervisor is the process that owns the session.
#   shutdown  systemctl poweroff / reboot through logind. That goes through
#   reboot    polkit, and 05-install-prefix.sh --systemd installs the rule that
#             lets the unprivileged session user do it without a password.
set -u

action=$(basename "$0")
case "$action" in
    serenity-logout | logout) action=logout ;;
    serenity-shutdown | shutdown | poweroff) action=shutdown ;;
    serenity-reboot | reboot) action=reboot ;;
    *)
        echo "session-control: unknown action '$action'" >&2
        exit 2
        ;;
esac

say() { echo "serenity-$action: $*" >&2; }

# The installed binary is libexec/serenitid.real and /proc/<pid>/comm holds the
# executable basename, so this needs neither pgrep nor pidof.
serenitid_pid() {
    for dir in /proc/[0-9]*; do
        [ -r "$dir/comm" ] || continue
        if [ "$(cat "$dir/comm" 2>/dev/null)" = "serenitid.real" ]; then
            echo "${dir#/proc/}"
            return 0
        fi
    done
    return 1
}

case "$action" in
    logout)
        if pid=$(serenitid_pid); then
            if kill -USR1 "$pid"; then
                exit 0
            fi
            say "cannot signal serenitid (pid $pid)"
            exit 1
        fi
        say "serenitid is not running - nothing to log out of"
        exit 1
        ;;
    shutdown | reboot)
        # logind is the only path that works from a non-root session, and
        # systemctl is the least surprising way to reach it. Its verb is
        # "poweroff", not "shutdown".
        verb=$action
        [ "$action" = shutdown ] && verb=poweroff
        if command -v systemctl >/dev/null 2>&1; then
            systemctl --no-ask-password "$verb" && exit 0
        fi
        # No systemd, or polkit refused: fall back to the plain command, which
        # only succeeds when the session already has the privilege.
        case "$action" in
            shutdown) exec shutdown -h now ;;
            reboot) exec reboot ;;
        esac
        say "$action failed"
        exit 1
        ;;
esac
