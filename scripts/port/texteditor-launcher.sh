#!/bin/sh
# "Text Editor": now the native SerenityOS LibGUI Notepad (serenity-notepad).
#
# The old shim delegated to a host GTK editor (mousepad), which crashed easily
# and ignored the port's Win9x look. The /bin/TextEditor -> serenity-text-editor
# name convention is kept, so the .af entry, quick-launch and the FileTypes=txt
# handler all keep working unchanged.
#
# Installed to <prefix>/bin/serenity-text-editor by 05-install-prefix.sh.

prefix="${SERENITY_PREFIX:-/serenity}"
exec "$prefix/bin/serenity-notepad" "$@"
