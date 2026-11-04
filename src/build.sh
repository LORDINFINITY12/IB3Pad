#!/bin/sh
# Builds xinput1_3.dll (64-bit) with the MinGW-w64 cross-compiler.
# Debian/Ubuntu: apt install gcc-mingw-w64-x86-64
# On Windows, MSYS2 MinGW64 works the same with CC=gcc.
set -e
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc}
$CC -O2 -s -Wall -Wextra -Wno-cast-function-type -shared -static-libgcc \
    -o xinput1_3.dll ib3pad.c xinput1_3.def -luser32 -lgdi32 -lwinmm -ladvapi32
echo "built $(pwd)/xinput1_3.dll"
