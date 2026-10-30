#!/bin/bash
# desktop simulator of the Android app (fake NativeActivity / window / AAudio / input)
# SIMINC: a folder with the NDK's android/, aaudio/ headers and jni.h (host-compilable copies)
set -e
cd "$(dirname "$0")/.."
bash test/build_linksim.sh >/dev/null
clang -O2 -w -DGB_INTERP_ONLY -Icore -Iandroid -I"${SIMINC:-/root/siminc}" test/sim.c android/cpubot.c android/rom.c \
  build/sim2/core_a.o build/sim2/core_b.o -lpthread -lm -o build/sim
echo built build/sim
