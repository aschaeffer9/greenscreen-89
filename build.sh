#!/bin/bash
# Full build: interpreter core + app shell -> arm64 .so -> signed, aligned APK (contains no game code or data).
set -euo pipefail
cd "$(dirname "$0")"
SYSROOT="${SYSROOT:-/root/sysroot}"
rm -rf build/arm64 build/apk; mkdir -p build/arm64 build/apk/lib/arm64-v8a

# No game code or data goes into the app: the player loads their own ROM on first run.
# The CPU core is a generic Game Boy instruction interpreter generated without any ROM.
python3 gen.py --interp core/cpu_interp.c
CF="-DGB_INTERP_ONLY --target=aarch64-linux-android28 --sysroot=$SYSROOT -fPIC -O2 -fvisibility=hidden -mno-outline-atomics -fno-stack-protector -Icore -Iandroid"
for f in android/main.c android/touchui.c android/link.c android/net.c android/cpubot.c android/rom.c core/gbcore.c core/tetris_extras.c core/cpu_interp.c; do
  clang $CF -Wall -Wno-unused-function -c "$f" -o "build/arm64/$(basename "$f" .c).o"
done

# VS CPU: a second copy of the core with every symbol renamed b_*
mkdir -p build/coreb
ld.lld -r -o build/coreb/core_a.o build/arm64/cpu_interp.o build/arm64/gbcore.o build/arm64/tetris_extras.o
NM=llvm-nm-18; command -v $NM >/dev/null || NM=/usr/lib/llvm-18/bin/llvm-nm
OC=llvm-objcopy-18; command -v $OC >/dev/null || OC=/usr/lib/llvm-18/bin/llvm-objcopy
$NM -g --defined-only build/coreb/core_a.o | awk 'NF==3{print $3" b_"$3}' > build/coreb/rename.txt
$OC --redefine-syms=build/coreb/rename.txt build/coreb/core_a.o build/arm64/core_b.o

# link against stub libs (only symbol names matter; real libs are on the device)
N=llvm-nm-18; command -v $N >/dev/null || N=/usr/lib/llvm-18/bin/llvm-nm
UND=$($N -u build/arm64/*.o | grep -v ':' | awk '{print $2}' | sort -u)
DEF=$($N --defined-only build/arm64/*.o | grep -v ':' | awk '{print $3}' | sort -u)
mkdir -p build/stubs; : > build/stubs/libc.c; : > build/stubs/libandroid.c; : > build/stubs/libaaudio.c; : > build/stubs/liblog.c
for s in $(comm -23 <(echo "$UND") <(echo "$DEF")); do
  case $s in AAudio*) f=libaaudio;; __android_log*) f=liblog;; A*) f=libandroid;; *) f=libc;; esac
  echo "void $s(void){}" >> build/stubs/$f.c
done
for l in c android aaudio log; do
  clang --target=aarch64-linux-android28 -w -fPIC -shared -nostdlib -Wl,-soname,lib$l.so -o build/stubs/lib$l.so build/stubs/lib$l.c
done
ld.lld -shared -soname libgreenscreen.so --no-undefined -z now -z relro -z noexecstack -z max-page-size=16384 \
  --hash-style=both --build-id=sha1 --gc-sections -o build/apk/lib/arm64-v8a/libgreenscreen.so \
  build/arm64/*.o -Lbuild/stubs -lc -landroid -laaudio -llog

python3 android/mkapk.py build/apk/lib/arm64-v8a/libgreenscreen.so android/icon.png build/gs-unsigned.apk
KS="${KEYSTORE:-build/greenscreen.keystore}"; KP="${KEYSTORE_PASS:-greenscreen89}"
[ -f "$KS" ] || keytool -genkeypair -keystore "$KS" -storepass "$KP" \
  -keypass "$KP" -alias greenscreen -keyalg RSA -keysize 2048 -validity 20000 -dname "CN=Greenscreen 89 Personal Build"
cp build/gs-unsigned.apk build/gs-signed.apk
jarsigner -keystore "$KS" -storepass "$KP" -sigalg SHA256withRSA -digestalg SHA-256 \
  build/gs-signed.apk greenscreen >/dev/null
python3 android/zipalign.py build/gs-signed.apk build/Greenscreen89.apk
jarsigner -verify build/Greenscreen89.apk | grep -q "jar verified" && echo "APK OK: build/Greenscreen89.apk"
