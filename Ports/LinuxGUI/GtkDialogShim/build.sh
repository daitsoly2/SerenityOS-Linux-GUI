#!/bin/sh
# 定制 GTK3 文件选择器重定向库。
# 在目标机器上编译（不交叉编译）：这个 .so 只依赖 libc/libdl，GTK/GLib 符号
# 全部运行时 dlsym，所以其实与 libgtk-3 版本无关，但同机编译最省心。
set -e
cd "$(dirname "$0")"
gcc -shared -fPIC -O2 -Wall -Wextra -Wno-pedantic \
    -o libserenity-gtk3-dialog.so serenity-gtk3-dialog.c -ldl
echo "--- DT_NEEDED（必须只有 libc，不能有 libgtk-3）："
ldd libserenity-gtk3-dialog.so
echo "--- 导出符号："
nm -D --defined-only libserenity-gtk3-dialog.so | grep -E "gtk_dialog_run| [Tt] " | head
