# Greenscreen '89 — iOS port

A sideloadable iOS build of the app. Same emulator core, same shell code, a
different platform layer: `ios/shim.c` implements the small Android API surface
`android/main.c` was written against (`ANativeWindow`, `AInputQueue`,
`ALooper`, `AAudio`, the activity callbacks), and `ios/main.m` is the UIKit
host. Nothing in `core/`, `android/main.c`, `android/touchui.c`,
`android/link.c`, `android/cpubot.c` or `android/rom.c` is modified.

This is the same trick as `test/sim.c`, which fakes that API on the desktop.

## Build

On a Mac with Xcode:

```sh
./ios/build_ios.sh
# -> build/Greenscreen89-unsigned.ipa
```

No Xcode project, no CocoaPods, no Gradle: clang, a hand-written `Info.plist`
and a zip. On GitHub, `.github/workflows/ios.yml` does the same on a
`macos-26` runner and uploads the `.ipa` as an artifact.

Options: `PACKAGE=com.you.greenscreen89`, `VERSION_NAME=3.5`,
`VERSION_CODE=18`, `MIN_IOS=15.0`.

## Install

1. Download the `.ipa` from the workflow artifact (or build it yourself).
2. Sideload it with [Sideloadly](https://sideloadly.io/), AltStore or
   SideStore, signing with your own Apple ID.
3. Trust the developer certificate in *Settings → General → VPN & Device
   Management*.

With a free Apple ID the signature lasts **7 days** and you can have 3 apps
installed; re-sideload to renew. A paid developer account ($99/yr) gets a year
per signing, or TestFlight (90 days per build, no cable). AltStore/SideStore can
renew on a schedule so you rarely think about it.

## Distributing

The `.ipa` is device-agnostic and contains no game data, so publish the
**unsigned** build: drop the `ios/` folder in the repo, push a `v*` tag, and
the workflow attaches `Greenscreen89-unsigned.ipa` to the GitHub Release.
Everyone who downloads it signs it with their own Apple ID at install time —
that is why the file can be public and why signing in CI would be pointless
(it would only run on the builder's devices).

Prefer Releases over Actions artifacts: artifacts expire and need a login.
Never commit a signed `.ipa`, and never commit a ROM.

## Loading the ROM

The app still ships no game. The app's Documents folder is visible in the
Files app (*On My iPhone → Greenscreen89*) because `UIFileSharingEnabled` is
set.

- Drop `rom.gb` (or a `.zip` containing it) into that folder, then either wait
  a moment — the app polls for `rom.gb` — or tap **FIND IN DOWNLOADS** on the
  first screen, which now scans the app's own folder.
- The ROM is checked against *Tetris (World) (Rev A)* and kept privately in
  the same folder afterwards.

## What works

- Solo play: rendering, touch gestures, audio, save state, pause/resume,
  settings (sensitivity, flick, key bindings — the binding UI works, but see
  below), soft reset, the whole menu system.
- iOS lifecycle: backgrounding pauses and saves, returning resumes.
- Screen stays awake while playing; status bar and home indicator are hidden.

## What is not in this build

- **Link cable** (same Wi-Fi, USB-C, online). The menu opens and fails with
  `NO LINK IN THIS BUILD` instead of searching forever:
  - Wi-Fi pairing used UDP broadcast, which iOS gates behind the
    `com.apple.developer.networking.multicast` entitlement (not available on a
    free ID). The replacement is Bonjour discovery + unicast.
  - There is no iOS equivalent of Android's USB-C accessory link.
  - Online play needs MQTT over TLS; the Android build borrows Java's
    `SSLSocket` through JNI, so iOS needs a TLS stack (`Network.framework` or
    vendored mbedTLS). `ios/net_stub.c` is the placeholder.
- **Update check** (`ios/update_ios.c` is a no-op).
- **Trackpad/pointer capture**, hardware keyboard and gamepad input. The shim
  accepts `AInputEvent`s for them already; nothing generates them yet.

## Notes

- Frames are presented with `CGImage` + `CALayer.contents` from three rotating
  buffers, so there is no per-frame copy in the shim. A Metal path would cut
  GPU upload cost if it ever matters.
- Audio is `AudioQueue` (`AVAudioSessionCategoryAmbient`, so it mixes and obeys
  the ring/silent switch). `AAudioStream_write()` blocks on the ring buffer,
  which is what paces the game loop, exactly like the Android build.
- `rom.c` is compiled with `-Drom_scan=rom_scan_android`; `ios/rom_ios.c`
  provides the real `rom_scan()` pointed at Documents.
- The VS CPU second core is produced without `objcopy --redefine-syms`:
  `build_ios.sh` recompiles the core with `-Dname=b_name` for every global the
  first copy defines.
