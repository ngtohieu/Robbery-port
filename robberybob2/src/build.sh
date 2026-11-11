#!/bin/sh
# Cross-compiles rb2loader for aarch64 glibc >= 2.17 with zig (pip install ziglang).
cd "$(dirname "$0")"
python -m ziglang cc -target aarch64-linux-gnu.2.17 -O2 -g0 -s -Wall -Wno-pedantic \
  -fno-omit-frame-pointer -o ../rb2loader \
  main.c so_loader.c bionic.c jni.c audio.c zip.c sdl.c gles1emu.c -ldl -lpthread -lm
