#!/bin/sh
# 编译 serenity-hostd（宿主 root 代理）。
#
# 这个守护是纯 Linux 程序：不依赖 AK/LibCore，也不进 Lagom 的 CMake 图 ——
# 它跑在宿主/VM 上而不是 Serenity 桌面里，独立编译可以避免每次改一行就触发
# CMake 重配置（那个流程在宿主上很容易因缺 libsdl2/libxcb-imdkit 而失败）。
#
# 用法：
#   Ports/LinuxGUI/Hostd/build.sh              # 默认输出 Build/hostd/serenity-hostd
#   OUT=/tmp/out CXX=clang++ .../build.sh
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${OUT:-"$ROOT/Build/hostd"}
CXX=${CXX:-c++}

mkdir -p "$OUT"

# -D_GNU_SOURCE 是为了 pipe2() 与 struct ucred（SO_PEERCRED）；g++ 默认就带，
# clang++ 只带 -std=c++17 时也带，这里显式写出以免换编译器后悄悄失效。
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter \
    -D_GNU_SOURCE \
    -o "$OUT/serenity-hostd" "$HERE/serenity-hostd.cpp"

echo "built: $OUT/serenity-hostd"
