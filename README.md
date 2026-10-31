# Greenscreen '89
<img width="192" height="192" alt="greenscreen-icon" src="https://github.com/user-attachments/assets/96bd3dcf-3f35-48fa-ba06-da3533a752a9" />

A touchscreen Android app (now with beta ios build) for playing the 1989 Game Boy Tetris from **your own cartridge**. It has swipe and flick controls, link-cable multiplayer (same Wi-Fi, USB-C phone to phone, or online) and a built-in CPU opponent.

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

**CONTROLS** (under the START button) opens two sliders:

- **Swipe sensitivity** (1–10): how far you drag per column and before soft drop starts. Higher means less movement.
- **Hard drop flick** (0–10): how short and slow a flick can be and still hard-drop. 0 turns hard drop off.

The panel also shows game speed and CPU load per frame, which is handy for checking performance on slower phones.

**HOME** (next to CONTROLS, or the Home key) goes straight back to the title screen. Mid-game, it asks first.

Keyboards and controllers work too (good for DeX). Every key can be changed in **CONTROLS > KEYBOARD KEYS**: tap an action, then press a key. Each action holds two keys.

| Default key | Action |
| --- | --- |
| Arrows / WASD | Move, soft drop (Up / W for menus) |
| Space, X | Rotate |
| Z, J | Rotate back |
| Alt | Hard drop |
| Enter | START |
| Shift, Tab | SELECT |
| Esc | Back |
| F1 | CONTROLS |
| F2 | Trackpad on / off |
| Home | Back to the title screen |

On controllers, R1 is hard drop.

**Trackpad mode** (CONTROLS, or F2) uses a laptop or DeX trackpad like the touch screen: drag to move, tap to rotate, flick down to hard drop. The pointer is captured while you play; Esc lets it go.

**Updates:** once per launch the app asks GitHub for the latest release. If a newer version is out, it offers to open the releases page. Nothing else is sent.

2PLAYER on the title screen opens the link menu:

- **VS CPU**: Easy, Normal or Hard bot. It runs on a second emulated Game Boy that the bot plays through its buttons.
- **Same Wi-Fi**: automatic pairing over UDP.
- **USB cable**: two phones with one USB-C cable (Android Open Accessory).
- **Online**: while the app is open you're listed as online. You show as *free* at the title screen, in menus, and between games, and as *busy* during a solo game. The ONLINE button shows a green dot and how many players are free right now. Tapping it asks free players one at a time; they get an ACCEPT / DECLINE prompt, and nobody is interrupted mid-game. Two players searching at once are matched automatically. After 2 minutes with no match, the search stops and says so. Matchmaking runs through public open-source MQTT brokers, then switches to a direct phone-to-phone connection when the networks allow it (badge shows `LINK P2P`); otherwise packets go through the broker. Turn the listing off with **SHOW ME ONLINE** in CONTROLS. **Custom server** accepts any TLS MQTT broker address from the clipboard.

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

To use your own relay instead of the public brokers, build with `EXTRA_CFLAGS='-DT89_WS_RELAY="your-relay.example.workers.dev"'` and deploy the Worker in `legacy/cloudflare-relay`.

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
./build/sim /tmp/out audio                                # sound survives app switching, device changes, menus (SIM_AUDIO_STRICT=1)
./build/sim /tmp/out onlineui                             # online dot, busy status, invitation prompt
bash test/mqtt/run.sh                                      # online connection: direct, relay, fallback
bash test/mqtt/presence_run.sh                             # who's free, invitations, declines, timeouts
```

## License

The project's own code is under the [MIT License](LICENSE). That covers this repository only, not the game, which belongs to its rights holders (see [CREDITS.md](CREDITS.md)).
