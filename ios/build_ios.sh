#!/bin/bash
# Build an unsigned .ipa on a Mac (or a GitHub macOS runner), no Xcode project.
# Everything is clang + a hand-written Info.plist, like build.sh does for the APK.
#
#   ./ios/build_ios.sh                       -> build/Greenscreen89-unsigned.ipa
#   SDK=iphonesimulator ./ios/build_ios.sh   -> build/ios-sim/Payload/...app (Simulator)
#   PACKAGE=com.you.greenscreen89 ./ios/build_ios.sh
#
# The .ipa is unsigned on purpose: Sideloadly / AltStore / SideStore sign it
# with your own Apple ID.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION_NAME="${VERSION_NAME:-3.4}"
VERSION_CODE="${VERSION_CODE:-17}"
PACKAGE="${PACKAGE:-com.example.greenscreen89}"
MIN_IOS="${MIN_IOS:-15.0}"
SDK="${SDK:-iphoneos}"

command -v xcrun >/dev/null 2>&1 || { echo "error: needs Xcode (xcrun not found)" >&2; exit 1; }

if [ "$SDK" = "iphonesimulator" ]; then
    OUT=build/ios-sim
    TARGET=(-target "arm64-apple-ios${MIN_IOS}-simulator")
else
    OUT=build/ios
    TARGET=(-miphoneos-version-min="$MIN_IOS")
fi
SDKROOT="$(xcrun --sdk "$SDK" --show-sdk-path)"
CC=(xcrun --sdk "$SDK" clang -arch arm64 "${TARGET[@]}" -isysroot "$SDKROOT")
CF=(-O2 -fno-strict-aliasing -DNDEBUG -DGB_INTERP_ONLY
    -DAPP_VERSION=\"$VERSION_NAME\"
    -Iios/include -Icore -Iandroid
    -Wno-unused-function -Wno-unused-parameter)

rm -rf "$OUT"
mkdir -p "$OUT/obj"

echo "== generating the CPU interpreter"
python3 gen.py --interp core/cpu_interp.c

echo "== core (copy A)"
for f in core/cpu_interp.c core/gbcore.c core/tetris_extras.c; do
    "${CC[@]}" "${CF[@]}" -c "$f" -o "$OUT/obj/$(basename "${f%.c}").o"
done

# VS CPU: a second copy of the core with every global symbol renamed b_*.
# Android does this with objcopy --redefine-syms; here the same effect comes
# from recompiling with -Dname=b_name, which needs no extra tool.
echo "== core (copy B, b_* prefix)"
xcrun --sdk "$SDK" nm -g "$OUT/obj/cpu_interp.o" "$OUT/obj/gbcore.o" "$OUT/obj/tetris_extras.o" \
    | awk 'NF >= 3 && $2 ~ /^[TDBSCR]$/ { print $3 }' | sed 's/^_//' | sort -u > "$OUT/syms.txt"
[ -s "$OUT/syms.txt" ] || { echo "error: no core symbols found" >&2; exit 1; }
: > "$OUT/rename.flags"
while IFS= read -r s; do
    printf -- '-D%s=b_%s\n' "$s" "$s" >> "$OUT/rename.flags"
done < "$OUT/syms.txt"
for f in core/cpu_interp.c core/gbcore.c core/tetris_extras.c; do
    "${CC[@]}" "${CF[@]}" $(cat "$OUT/rename.flags") -c "$f" -o "$OUT/obj/$(basename "${f%.c}")_b.o"
done

echo "== app"
for f in android/main.c android/touchui.c android/link.c android/cpubot.c ios/shim.c ios/rom_ios.c ios/update_ios.c ios/net_stub.c; do
    "${CC[@]}" "${CF[@]}" -c "$f" -o "$OUT/obj/$(basename "${f%.c}").o"
done
mv "$OUT/obj/main.o" "$OUT/obj/app_main.o"
# keep rom.c's scanner under another name: iOS reads the ROM from Documents
"${CC[@]}" "${CF[@]}" -Drom_scan=rom_scan_android -c android/rom.c -o "$OUT/obj/rom.o"

echo "== UIKit host"
"${CC[@]}" "${CF[@]}" -fobjc-arc -c ios/main.m -o "$OUT/obj/uikit_main.o"

echo "== link"
"${CC[@]}" -o "$OUT/Greenscreen89" "$OUT"/obj/*.o -fobjc-link-runtime \
    -framework UIKit -framework Foundation -framework QuartzCore -framework CoreGraphics \
    -framework AudioToolbox -framework AVFoundation -framework CoreFoundation \
    -lpthread -lm

echo "== bundle"
APP="$OUT/Payload/Greenscreen89.app"
mkdir -p "$APP"
cp "$OUT/Greenscreen89" "$APP/Greenscreen89"
sed -e "s/__PACKAGE__/$PACKAGE/" -e "s/__VERSION__/$VERSION_NAME/" \
    -e "s/__BUILD__/$VERSION_CODE/" -e "s/__MINIOS__/$MIN_IOS/" \
    ios/Info.plist > "$APP/Info.plist"
if command -v sips >/dev/null 2>&1; then
    sips -s format png --resampleHeightWidth 1024 1024 android/icon.png --out "$APP/Icon-1024.png" >/dev/null
fi

if [ "$SDK" != "iphoneos" ]; then
    echo "APP OK: $APP"
    exit 0
fi

echo "== ipa"
rm -f build/Greenscreen89-unsigned.ipa
( cd "$OUT" && zip -qry ../Greenscreen89-unsigned.ipa Payload )
echo "IPA OK: build/Greenscreen89-unsigned.ipa"
