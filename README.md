# Greenscreen '89

A touchscreen Android app for playing the 1989 Game Boy Tetris from **your own cartridge**. It has swipe and flick controls, link-cable multiplayer (same Wi-Fi, USB-C phone to phone, or online) and a built-in CPU opponent.

**No game is included.** This repository and the app contain no Nintendo code, graphics, music or data. On first launch the app asks for your ROM image and checks it against the fingerprint of the *Tetris (World) (Rev A)* cartridge (MD5 `982ed5d2b12a0377eb14bcdc4123744e`). Dump it from a cartridge you own.

Unofficial fan project, personal and non-commercial. Not affiliated with The Tetris Company, Tetris Holding, Nintendo or ELORG. See [CREDITS.md](CREDITS.md).

## Requirements

- Android 9 (API 28) or newer, on a 64-bit ARM phone (arm64-v8a). This covers nearly every Android phone from 2017 on.
- It installs normally on Android 14 through 17. They block apps targeting API 23 or 24 and older; this one targets 28.

## Playing

| Gesture | Action |
| --- | --- |
| Drag left / right | Move |
| Tap (right half / left half) | Rotate (A / B) |
| Hold down | Soft drop |
| Quick flick down | Hard drop |
| Tap menu items | Choose; Back = B |
| Hold START | Soft reset |

2PLAYER on the title screen opens the link menu:

- **VS CPU**: Easy, Normal or Hard bot. It runs on a second emulated Game Boy that the bot plays through its buttons.
- **Same Wi-Fi**: automatic pairing over UDP.
- **USB cable**: two phones with one USB-C cable (Android Open Accessory).
- **Online**: matchmaking through public open-source MQTT brokers, then a direct phone-to-phone connection when the networks allow it (badge shows `LINK P2P`). If not, packets go through the broker. **Custom server** accepts any TLS MQTT broker address from the clipboard.

## Loading your ROM

Any of these works on first launch:

1. **Find in Downloads**: asks for file access and searches Downloads and Documents for a `.gb` file or a `.zip` that contains one.
2. **Open with**: open the `.gb` or `.zip` from your Files app, or share it, and pick Greenscreen '89.

The app keeps a private copy, so this happens once.

## Building

There's no Gradle and no Android SDK. Everything is C plus a few Python scripts.

Needs: `clang` / `ld.lld` / `llvm-nm` / `llvm-objcopy` (LLVM 18), Python 3, a JDK (`keytool`, `jarsigner`), and the NDK headers.

```sh
SYSROOT=/path/to/ndk/toolchains/llvm/prebuilt/linux-x86_64/sysroot ./build.sh
# -> build/Greenscreen89.apk
```

`build.sh` generates a generic Game Boy CPU interpreter (`gen.py --interp`, which needs no ROM), compiles the arm64 library, builds a second renamed copy of the core for the CPU opponent, writes the manifest and resources by hand (`android/mkapk.py`), then signs and aligns the APK.

The signing key is created at `build/greenscreen.keystore` on the first build (or pass `KEYSTORE=` and `KEYSTORE_PASS=`). **Keep a backup.** Android only installs updates signed with the same key. Override the app ID with `PACKAGE=com.example.something ./build.sh`.

## Layout

| Path | What |
| --- | --- |
| `core/gbcore.c` | Game Boy hardware model: video, sound, timer, joypad, serial link, save states, look-ahead |
| `core/tetris_extras.c` | Game-aware helpers (RAM addresses from the disassembly): hard drop, link hints |
| `gen.py` | Generates the CPU interpreter. Its static-recompiler mode needs a ROM and the disassembly, and is for local experiments only; never commit its output. |
| `android/main.c` | NativeActivity shell: rendering, touch, audio, menus, link sessions, VS CPU |
| `android/link.c` | Link-cable protocol over unreliable transports |
| `android/net.c` | Transports: Wi-Fi UDP, USB accessory, MQTT matchmaking + STUN + UDP hole punching |
| `android/cpubot.c` | CPU opponent |
| `android/rom.c` | ROM import: MD5 check, zip/DEFLATE reader, Android intents |
| `android/touchui.c` | Tap targets for the game's menus |
| `test/` | Desktop simulator of the app, two-core link simulator, bot matches, online tests against a local broker |
| `legacy/cloudflare-relay` | The Cloudflare Worker used for online play by the 2.x builds |

## Tests

The tests need your own ROM. Point at it with `T89_ROM=/path/to/rom.gb`.

```sh
bash test/build_sim.sh && ./build/sim /tmp/out             # single-player script through the real app code
./build/sim /tmp/out cpu 2 1                               # VS CPU (Hard), one round
./build/sim /tmp/out norom                                 # first-run ROM screen
bash test/build_linksim.sh && python3 test/cpubot_match.py "$T89_ROM" 2 1 2   # bot vs bot over the link
bash test/mqtt/run.sh                                      # online matchmaking: direct, relay, fallback
```

## License

The project's own code is under the [MIT License](LICENSE). That covers this repository only, not the game, which belongs to its rights holders (see [CREDITS.md](CREDITS.md)).
