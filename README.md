<p align="center"><img src="assets/logo.png" width="128" alt="WinDrop logo"></p>

# WinDrop

AirDrop-style transfers between an Android phone and a Windows PC: files, text, links and the clipboard, both ways. It works on any network, even campus Wi-Fi that keeps devices apart, because the two connect directly over Wi-Fi Direct. No cables, no hotspot, no shared Wi-Fi.

## Download

| | |
|---|---|
| **Windows 10/11** | [WinDrop-Setup.exe](https://github.com/Skyblock127/WinDrop/releases/latest/download/WinDrop-Setup.exe) |
| **Android 10+** | [WinDrop.apk](https://github.com/Skyblock127/WinDrop/releases/latest/download/WinDrop.apk) |
| **Both** | [WinDrop.zip](https://github.com/Skyblock127/WinDrop/releases/latest/download/WinDrop.zip) |

## Setup

1. **PC:** run **WinDrop-Setup.exe**. Wi-Fi and Bluetooth must be on.
2. **Phone:** install **WinDrop.apk**, open it, and allow what it asks for.
3. **Pair:** PC tray icon → **Pair phone…**, then **Pair with PC** on the phone. If both screens show the same number, click **Yes** on the PC.
4. Add the **WinDrop** tile to the phone's Quick Settings (on/off switch).

**"Windows protected your PC"?** WinDrop isn't code-signed yet. Click **More info → Run anyway**. Windows 11's *Smart App Control* blocks unsigned apps outright; the only way past it is turning it off in Windows Security.

## Use

- **Phone → PC:** Share → **WinDrop**, or **Send clipboard to PC** in the app or notification.
- **PC → phone:** drag onto the pill at the top-center of the screen, Explorer → *Send to → Phone (WinDrop)*, or `Ctrl+Alt+P` for the clipboard.
- **Several phones:** pair up to 4 (tray → **Phones → Pair another phone…**). Dragging then shows one drop spot per phone; `Ctrl+Alt+P` and *Send to* go to the phone you used last.
- Files land in `Downloads\WinDrop` on both sides; text and links go to the clipboard.
- Anything sent while the other side is out of reach waits and goes out by itself (PC: tray → **Waiting to send**; phone: **Not sent yet** in the app).

## Updates

Both apps check this repo's releases and offer new versions (PC: tray → **Update**; phone: notification and **Update** in the app). Downloads are verified against GitHub's SHA-256 before installing. A phone and PC with incompatible versions refuse to connect and say which one to update.

## Security

Pairing is a P-256 key exchange confirmed by a 6-digit number on both screens. After that, every connection is mutually authenticated and encrypted with AES-256-GCM. Unpaired devices are refused, and the PC accepts pairing only while **Pair phone…** is open.

## Build

- **Windows:** Visual Studio 2022 Build Tools (C++, Windows SDK) and CMake: `cmake -S windows -B windows/build-msvc -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release && cmake --build windows/build-msvc` from a VS x64 prompt.
- **Android:** `cd android && ./gradlew assembleRelease` (JDK 17+). Release signing reads `~/.windrop-signing/keystore.properties`; without it, the debug key is used.
- **Release:** bump `VERSION`, commit, then `tools\release.ps1 -Publish` (needs Inno Setup 6 and `gh`). It builds both apps and the installer and publishes them as a GitHub release.

## Something not working?

Collect logs on both sides and send them along:

1. **Clear logs** (PC: tray icon, Logs; phone: WinDrop app, Clear logs).
2. Try the thing that fails once more.
3. **Save logs** on both. The files land in `Downloads\WinDrop` (PC) and `Downloads/WinDrop` (phone). They include a check of firewall, antivirus, VPN and radio settings.

Most pairing failures where the phone says "Couldn't reach your PC" are a firewall or antivirus on the PC. In Windows Security, Firewall & network protection, Public network, "Blocks all incoming connections" must be off. An antivirus with its own firewall needs WinDrop allowed too.

## License

[MIT](LICENSE)
