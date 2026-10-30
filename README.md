<p align="center"><img src="assets/logo.png" width="128" alt="WinDrop logo"></p>

# WinDrop

AirDrop-style transfers between an Android phone and a Windows PC: files, text, links and the clipboard, both ways. It works on whatever network each device is on, including campus Wi-Fi that keeps devices apart, because the two talk directly over **Wi-Fi Direct**, opened only while something is being sent. **Bluetooth LE** lets the PC wake the phone. Each side has a small pill at the top of the screen to drop things on.

## Download

| | |
|---|---|
| **Windows 10/11** | [**WinDrop-Setup.exe**](https://github.com/Skyblock127/WinDrop/releases/latest/download/WinDrop-Setup.exe) |
| **Android 10+** | [**WinDrop.apk**](https://github.com/Skyblock127/WinDrop/releases/latest/download/WinDrop.apk) |
| **Both, plus install notes** | [WinDrop.zip](https://github.com/Skyblock127/WinDrop/releases/latest/download/WinDrop.zip) |

Older versions and release notes: [Releases](https://github.com/Skyblock127/WinDrop/releases).

## Install

You need a Windows PC with Wi-Fi and Bluetooth, and an Android 10+ phone.

1. **PC:** run **WinDrop-Setup.exe**. It asks where to install, adds WinDrop to the Start menu and the firewall, and can start it with Windows.
2. **Phone:** open **WinDrop.apk** and install it (allow "install unknown apps" for your browser or file manager if asked). Open WinDrop and allow what *Needs your OK* lists: notifications, Nearby devices, Bluetooth, the pill (*Display over other apps*), and the battery exemption.
3. **Pair:** on the PC, tray icon → **Pair phone…** (open for 3 minutes); on the phone, tap **Pair with PC**. Both screens show the same 6-digit number: click **Yes** on the PC if they match.
4. Add the **WinDrop** tile to the phone's Quick Settings. It's the on/off switch.

### "Windows protected your PC" / Smart App Control

WinDrop isn't code-signed yet (a signing certificate is a paid yearly subscription), so Windows doesn't know the publisher:

- **SmartScreen** ("Windows protected your PC"): click **More info → Run anyway**. It's a reputation check, not a virus finding.
- **Smart App Control** ("Smart App Control blocked an app", Windows 11): it blocks every unsigned app and has no "Run anyway". The only way past it is turning it off in Windows Security → App & browser control, and Windows won't turn it back on without a reset. Only do that if you're comfortable with it.

### Updates

Both apps update themselves from this repository's releases:

- **PC:** WinDrop checks a few seconds after it starts and every 6 hours, and the pill says when there's a new version. Tray icon → **Update to v…** downloads it, checks its SHA-256, and runs the installer (Windows asks for permission once). WinDrop restarts by itself.
- **Phone:** WinDrop checks once a day and posts a notification. In the app, **Update to …** downloads the APK, checks its SHA-256, and Android asks you to confirm. The first time, Android asks you to allow WinDrop to install apps.

A phone and PC only talk when their apps speak the same protocol version. If they don't, pairing and transfers are refused, and both sides say which one needs the update.

## Daily use

| | How |
|---|---|
| **On/off: phone** | Quick Settings tile (long-press opens the app) |
| **On/off: PC** | Tray menu → **On** (the icon greys out when off) |
| **Phone → PC** | Share → **Send to PC** (or the direct-share target), or drop onto the pill |
| **PC → Phone** | Drag files/text to the top-center of the screen and drop on the pill; Explorer → Send to → *Phone (WinDrop)*; or `Ctrl+Alt+P` to send the clipboard |

The tray menu has only: status, **On**, **Show pill**, **Send clipboard to phone**, **Waiting to send** (the queue: click an item to remove it, or **Clear queue**), **Pair phone…**, **Open received folder**, **Start with Windows**, **Check for updates**, **Quit**. The installer puts WinDrop in the Start menu; the app keeps Explorer's *Send to → Phone (WinDrop)* pointing at itself.

**Start with Windows** is the standard per-user `Run` registry entry, the same one listed in Settings → Apps → Startup and in Task Manager → Startup apps. Switching it off in either place is respected, and the tray tick follows it.

**The pill** (both sides) shows for about 3.5–4 s and then gets out of the way. A transfer keeps going, and on the phone its progress stays in the notification.
- **Phone:** the pill sits just below the status bar. Swipe it up to dismiss it now. A transfer request you swipe away moves to the notification.
- **PC:** flick the pill up or right-click it to dismiss it. Turn **Show pill** off in the tray to work with no pill at all; only a request from an unknown device still pops up. While you drag a file near the top-center, a wide invisible band along the top edge accepts the drop, so you don't need to aim at the pill.

- **Received files:** saved to `Downloads\WinDrop` on both sides. Text and links go to the clipboard.
- **Progress:** shown on the pill, and on the phone also in the WinDrop notification (with a bar).

### What happens on a transfer (measured on a OnePlus 11R + Intel AX201)

- **PC → phone:** the PC queues the item and starts its BLE beacon. Dragging toward the pill starts it even before you drop. The phone hears it in about 1 s, finds the PC, and connects; Windows pairs it in about 5 s. The transfer then runs at about 7–10 MB/s.
  - **Screen off:** OxygenOS holds Bluetooth scan results until the screen turns on, so the item arrives a few seconds after you pick up or wake the phone.
- **Phone → PC:** sharing opens the link straight away; the text or file is sent about 7 s later.
- **After a transfer:** the link stays open for 20 s, so a second item goes out at once. The phone then says goodbye and both sides close. A send right as it closes takes about 13 s.
- **Wi-Fi:** the phone normally keeps its own Wi-Fi throughout. If the two Wi-Fi channels clash, Android asks to pause Wi-Fi for the transfer and restores it afterwards.
- **Can't connect:** the item waits ("Waiting to send" on the phone, "Connecting…" on the PC pill). The phone stops trying after 90 s; *Send now* in the app retries.

## Logs

All logs end up in `%LOCALAPPDATA%\WinDrop\windrop.log` on the PC. That file is capped at about 10 MB (it rolls over to `windrop.log.1` at 5 MB). The phone keeps its recent log lines in memory and uploads them whenever a link is up; they appear with a `[phone]` prefix. On the phone itself: `adb logcat -s WinDrop`.

## Why it's built this way (Wi-Fi Direct quirks)

- **The PC accepts, the phone connects.** The phone sends a push-button connect request, the PC accepts it in-app with no Windows dialog and hosts the group (GO intent 15 vs 0).
- **Finding the PC by name.** Android doesn't expose a peer's vendor IEs, so the phone recognises the PC by its Wi-Fi Direct name, which it learns from the BLE beacon.
- **Temporary groups only.** Android would otherwise save the group and later try to rejoin it with an invitation. Windows refuses those ("no common channel"), and Android then offers to drop Wi-Fi. `netId = -1` is set via the config parcel because there's no public setter. As a result Windows re-pairs on every link, which costs about 5 s.
- **The PC re-advertises.** Windows stops answering Wi-Fi Direct searches a few minutes after it starts advertising, so the PC restarts the advertisement every 90 s while no phone is connected, and straight away when it needs the phone.
- **One BLE scan, never restarted.** Android silently stops delivering results to an app that starts more than 5 scans in 30 s. The phone runs one filtered scan with ALL_MATCHES and de-duplicates sightings itself.
- **Fast search on the phone.** Where supported (Android 14+), the phone searches only the 3 social channels the PC listens on, and repeats the search every 6 s.
- **Recovering from an older build:** if an old group shows in the phone's *Settings → Wi-Fi → Wi-Fi Direct → Remembered groups*, forget it once.

## Protocol (both apps)

TCP 53318 over the Wi-Fi Direct link, one connection per item, big-endian. Every connection opens with a version envelope whose shape never changes, so any two releases can at least tell each other they don't match:

```
envelope  C→S "WDRP" | proto u8 | type u8 | verLen u8 | app version | type's body      (proto is 3 today)
          S→C status u8: 01 go on · 00 refused · FE incompatible, then proto u8 | verLen u8 | version, and close
```
`proto` changes only when the bytes on the wire change; app versions with the same `proto` work together. After `01`:

```
session   C→S envelope(01) | idLen u8 | id | nC[16]
          S→C 01 | nS[16] | HMAC(K, "WDR2 srv"|nC|nS)        C→S HMAC(K, "WDR2 cli"|nC|nS)
          then AES-256-GCM records: u32 len | ciphertext | tag16; keys HKDF(K, nC|nS, "WDR2 c2s"/"WDR2 s2c")
pair      C→S envelope(02) | pubP[65] | nP[16] | id | name     (only while "Pair phone…" is open)
          S→C 01 | pubS[65] | nS[16] | id | name      (00 = refused)
          T = SHA256("WDR2 pair"|m1|m2), K = HKDF(ECDH, T, "WDR2 key"), check number = HKDF(ECDH, T, "WDR2 sas") mod 10^6
          S→C 01 | HMAC(K,"WDR2 pc-ok"|T)   C→S HMAC(K,"WDR2 phone-ok"|T)   S→C 00
```
Inside the encrypted session:

```
OFFER     "TDRP" | ver=1 u8 | type u8 | size u64 | nameLen u16 | senderLen u16 | idLen u16 | name | sender | deviceId
          type: 1 file · 2 text · 3 phone log lines · 4 bye (phone is closing the link)
DECISION  u8   1 accept · 2 decline · 3 busy
STREAM    size bytes (1 MB chunks, 1 MB records)
ACK       u8   0 ok · 0xFF incomplete
```
- **BLE beacon from the PC:** manufacturer data, company `0xFFFF`.
  - `"WD" + HMAC(K, window)[6] + 01` means "connect, I have something".
  - `"WP" + 00 + name` is sent only while the pairing window is open, so a pairing phone can find the PC.

## Security

- **Pairing:** happens once. The phone and PC run a P-256 key exchange over the Wi-Fi Direct link. It's only accepted while "Pair phone…" is open on the PC; the phone finds that PC by its pairing beacon. Both screens show a 6-digit check number derived from the exchange, and the user confirms on the PC. Both sides then keep a 32-byte key: DPAPI-protected on the PC, Android-Keystore-wrapped on the phone.
- **Every connection:** both sides prove they hold the key (HMAC challenge-response). Then everything is sent as AES-256-GCM records with per-connection keys. Anything else is refused: there's no trust-on-first-use, and no prompts for unknown devices.
- **The Wi-Fi Direct link:** Windows accepts it at the radio level from anyone. The PC ignores connection requests when it has no paired phone and isn't pairing, and drops any link that hasn't passed the key check within 15 s.
- **BLE beacon:** carries a 6-byte HMAC tag of the 10-minute time window, not the PC's name. The name only goes out during the pairing window.
- **Network presence:** no UDP broadcasts. When switched off, the PC closes its listening socket and sends nothing.
- **What remains:**
  - Windows always advertises the computer name over Wi-Fi Direct.
  - Someone nearby can see that a transfer happens, though not what is sent.
  - Pairing depends on the user actually comparing the numbers.
  - Anyone with the unlocked device can use it.
  - The binaries aren't code-signed.

## Known limits

- **Folders:** you can't send them. Zip the folder first.
- **Cross-app drag on the phone:** dropping onto the phone's pill only works from apps that allow cross-app drag.
- **One phone per PC** for now.

## Repository layout

```
android/     Kotlin app, no dependencies (com.windrop.app, minSdk 29, targetSdk 36)
windows/     C++20 Win32 tray app (GDI+, OLE drag-drop, Winsock, WinHTTP, C++/WinRT for Wi-Fi Direct + BLE)
installer/   Inno Setup script for WinDrop-Setup.exe, wizard images, the install notes in the zip
assets/      logo.png, the source of every icon
tools/       make_icons.py (all icons from the logo), release.ps1 (build + publish a release)
VERSION      the one version number; CMake and Gradle both read it
```

## Build from source

- **Windows app:** Visual Studio 2022 Build Tools ("Desktop development with C++" + Windows 11 SDK), CMake.
  ```bash
  cmd /c "\"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat\" && cmake -S windows -B windows/build-msvc -G \"NMake Makefiles\" -DCMAKE_BUILD_TYPE=Release && cmake --build windows/build-msvc"
  ```
- **Android app:** Android Studio's JDK (or any JDK 17+) and the Android SDK.
  ```bash
  cd android && ./gradlew assembleRelease
  ```
  Release builds are signed with the key in `~/.windrop-signing/keystore.properties` (`storeFile`, `storePassword`, `keyAlias`, `keyPassword`; or point `WINDROP_SIGNING` at another file). Without it they fall back to the debug key, so anyone can build, but such an APK can't update an official install.
- **Icons:** `python tools/make_icons.py` (needs Pillow) after changing `assets/logo.png`.

## Releasing

1. Bump `VERSION` (and the protocol number `kProto` / `PROTO` in `windows/src/Crypto.h` and `android/.../Crypto.kt`, only if the wire format changed).
2. Commit, then run `powershell -ExecutionPolicy Bypass -File tools\release.ps1 -Publish`. It builds both apps and the installer (needs Inno Setup 6), makes `dist\WinDrop.zip`, and creates the GitHub release `v<VERSION>` with `WinDrop-Setup.exe`, `WinDrop.apk` and `WinDrop.zip`. The installed apps pick it up as an update.

Keep the signing key safe and backed up: Android only installs an update signed with the same key.

## License

[MIT](LICENSE)
