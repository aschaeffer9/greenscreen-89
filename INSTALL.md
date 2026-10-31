# Install on iPhone or iPad

Greenscreen '89 is an unofficial fan port and includes no game. You install it
yourself and load your own Tetris cartridge dump.

**You need:** an iPhone or iPad on iOS 15 or newer, a computer with a USB cable
(Windows or Mac), a free Apple ID — the one you already use for iCloud is fine
— and your own *Tetris (World) (Rev A)* ROM.

---

## 1. Download the app

Go to the [Releases](../../releases) page and download
`Greenscreen89-unsigned.ipa`. Save it anywhere; you do not need to unzip it.

## 2. Install Sideloadly on your computer

Download it from [sideloadly.io](https://sideloadly.io/) (Windows or macOS) and
open it.

## 3. Put the app on your phone

1. Plug the iPhone into the computer, unlock it, and tap **Trust** if asked.
2. Drag the `.ipa` file into the Sideloadly window.
3. Type your Apple ID into the **Apple ID** box. It goes to Apple to sign the
   app — nobody else sees it.
4. Click **Start** and wait for it to say **Done**.

> If Apple asks for a password, use an *app-specific password* from
> [appleid.apple.com](https://appleid.apple.com) instead of your normal one.

## 4. Trust the app

On the iPhone:

- **Settings → General → VPN & Device Management →** tap your Apple ID **→ Trust**
- **iOS 16 and newer:** **Settings → Privacy & Security → Developer Mode →** turn
  it on, then restart the phone.

## 5. Add your ROM

1. Open the **Files** app → **On My iPhone** → **Greenscreen89**.
2. Copy your `rom.gb` (or a `.zip` containing it) into that folder.
3. Open the app. It picks the ROM up on its own, or tap **FIND IN DOWNLOADS**.

The file must be *Tetris (World) (Rev A)* — MD5 `982ed5d2b12a0377eb14bcdc4123744e`.
Anything else is rejected, because the port relies on that exact version.

---

## The 7-day thing

Apps signed with a free Apple ID stop working after **7 days**. When the app
refuses to open, run step 3 again — your ROM, saves and settings are kept.

If that gets annoying, two ways out:

- **[SideStore](https://sidestore.io/) or [AltStore](https://altstore.io/)**
  renew the app on the phone itself, mostly automatically.
- **A paid Apple Developer account** ($99/year) signs for a year at a time.

## If something goes wrong

| What you see | What to do |
|---|---|
| "Unable to Install" | Turn on Developer Mode (step 4), restart, try again |
| "Untrusted Developer" | Do step 4 |
| App opens, then closes on launch | The 7 days are up — do step 3 again |
| "That Tetris is a different version" | Wrong ROM; you need Rev A (MD5 above) |
| The app finds no ROM | Make sure `rom.gb` is a file inside the Greenscreen89 folder, not a folder |

## Not in this build yet

Link cable and online play work on Android but are not in the iOS build — the
2-player menu says so and closes. Solo play, sound, save states and the whole
menu system work.
