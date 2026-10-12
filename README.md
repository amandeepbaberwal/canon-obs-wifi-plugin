# canon-obs-wifi-plugin

Free, independent Wi-Fi webcam for the **Canon EOS M50 (original, not Mark II)** as a native OBS source plugin — no Canon EOS Webcam Utility, no camera hotspot, no losing internet. Camera and PC both stay on the existing router LAN.

Also includes the `bridge/` fallback: a PTP/IP → MJPEG bridge (`http://127.0.0.1:8787/live.mjpg`) for OBS Browser Source.

## How it works

- Speaks Canon PTP/IP on TCP `15740`: Init Command (90s pairing window) → Init Event →
  MTP session → Canon EOS bootstrap (`0x9114/0x9115/0x913d`/`0xd11c`/`0x911a`/EVF) →
  `GetViewFinderData (0x9153)` polling with `0xa102` warm-up handling.
- Key finding: DIGIC-8 EVF output `0xd1b0` is an enum (`0 none / 1 camera / 2 host /
  3 cameraAndHost`). The plugin writes `3` so the LCD stays live while frames go to
  the PC. Verified output: **up to 1024x576 MJPEG** (feed varies by session:
  480x320, 960x640, 1024x576 observed; camera limitation, OBS scales it).
- SSDP discovery for `SmartPhoneEOSSystemService` / `RemoteControlEOSSystemService` /
  `WFTEOSSystemService`, UPnP descriptor verify, auto-pick of the router-facing PC
  interface, CloseSession teardown, 10s reconnect backoff.
- JPEG decode via system WIC; no Canon binaries, drivers, or licensing touched.

## OBS plugin (`plugin/`)

1. Camera: mode dial `P`, Live View enabled, Wi-Fi function → **Connect to smartphone**
   → join the same router → waiting screen. Note the camera IP (Connection Information).
2. Build (Windows, VS 2022 Build Tools + CMake + obs-studio source at 32.2.2):
   ```bat
   cmake -S plugin -B build -G "Visual Studio 17 2022" -A x64 ^
     -DOBS_STUDIO_DIR=C:\path\to\obs-studio -DOBS_BUILD_DIR=C:\path\to\obs-build
   cmake --build build --config Release
   ```
   Defaults in `plugin/CMakeLists.txt` point at `C:\Users\aman\obs-studio` /
   `C:\Users\aman\obs-build` (libobs `obs.lib` + `w32-pthreads`).
3. Copy `build\Release\obs-m50-source.dll` to
   `C:\Program Files\obs-studio\obs-plugins\64bit\` (admin).
4. OBS → Sources **+** → **M50 Wi-Fi Camera** → Properties → Search cameras (or type IP)
   → Pair now → accept the camera prompt within 90s.

Prebuilt DLLs are attached to GitHub Releases.

## Bridge fallback (`bridge/`)

```bat
cd bridge
bun install
bun run dev -- --interfaces
scripts\run-pair.cmd --camera-ip 192.168.1.53 --local-address 192.168.1.43
scripts\run-bridge.cmd --camera-ip 192.168.1.53 --local-address 192.168.1.43 --evf-output 3
```

OBS Browser Source: `http://127.0.0.1:8787/live.mjpg`. Dashboard: `http://127.0.0.1:8787/`.
See `bridge/docs/m50-lan.md` for the full router-LAN setup. The T100/AP contract is
preserved (default `--evf-output 8`).

## Status / limits

Working: pairing, P + Movie mode live view in OBS, reconnect, SSDP search. Limits:
video-only
(no audio), fixed preview resolution, camera must stay awake on the waiting screen.
M50 support is hardware-verified on this setup; other bodies are unverified.

Independent project, not affiliated with Canon Inc.
