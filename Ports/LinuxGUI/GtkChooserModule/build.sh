#!/bin/sh
# 在 VM 上编译（GTK3 的 module 必须和运行时 libgtk-3 同机编译，避免版本漂移）。
set -e
cd "$(dirname "$0")"
g++ -shared -fPIC -O2 -o libserenity-filechooser.so serenity-filechooser.cpp \
    $(pkg-config --cflags --libs gtk+-3.0 gio-2.0 gmodule-2.0)
ls -l libserenity-filechooser.so
