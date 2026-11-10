#!/bin/sh
# Regenerate serenity_menu_v1 stubs from the XML.
# Run from anywhere: ./Ports/LinuxGUI/WlrootsHost/protocol/gen.sh
# Generated files are committed so build machines do not need wayland-scanner.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
XML="$HERE/serenity-menu-v1.xml"

command -v wayland-scanner >/dev/null 2>&1 || {
    echo "error: wayland-scanner not found (apt install wayland-protocols)" >&2
    exit 1
}

wayland-scanner private-code  "$XML" "$HERE/serenity-menu-v1-server.c"
wayland-scanner server-header "$XML" "$HERE/serenity-menu-v1-server.h"
wayland-scanner private-code  "$XML" "$HERE/serenity-menu-v1-client.c"
wayland-scanner client-header "$XML" "$HERE/serenity-menu-v1-client.h"
echo "generated: server.c/.h client.c/.h from $(basename "$XML")"
