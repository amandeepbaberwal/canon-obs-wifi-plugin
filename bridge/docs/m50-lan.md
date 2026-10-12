# M50 Router-LAN Mode (Unverified — Test Harness)

This document covers the **required** networking mode for the original Canon
EOS M50: camera and PC both join the existing router as clients. The PC keeps
internet access. Rivet never asks Windows to join a camera hotspot and never
changes Windows network settings.

> Status: **M50 Live View over LAN is unverified.** The T100 / 4000D Easy
> Connection path remains the only verified contract. This page provides the
> smallest testable LAN probe plus the next experiment if PTP/IP fails.

## 1. Camera setup (join the router)

On the original EOS M50 (not Mark II):

1. Mode dial: `P`.
2. `MENU` → Live View shooting: Enable.
3. `MENU` → Wi-Fi settings → Wi-Fi: Enable.
4. Wi-Fi function → smartphone icon (Camera Connect path first; it uses the
   same SSDP service Rivet already speaks).
5. Register a device → Infrastructure mode → join your home SSID → enter
   password → Auto IP (DHCP).
6. On the camera, open Connection Information and write down the camera IPv4
   (for example `192.168.1.60`). Leave the camera on the
   "waiting for application" screen and accept the pairing prompt when Rivet
   connects. Saved profiles reconnect without the prompt.

If the M50 only offers Camera access point mode on this firmware, stop here
and record the firmware version: router-LAN Live View is not available in
that mode and the next experiment is the EOS Utility (PC remote-control)
path, which is a different SSDP service and pairing flow.

Alternative PC path: Wi-Fi function → Remote control (EOS Utility) → join the
same router. Rivet `--discover` listens for the smartphone service, the
hypothesized remote-control service, and the observed M50 Utility service
`urn:schemas-canon-com:service:ICPO-WFTEOSSystemService:1` (hardware-verified
descriptor, 2026-10-12) so the mode is visible in diagnostics.

## 2. PC setup (keep internet)

1. Keep Windows connected to the same router SSID. Do not join a camera SSID.
2. Find the PC router IPv4 (example `192.168.1.43`):

```bat
bun run dev -- --interfaces
```

3. Run the LAN check (no Live View, no firewall changes):

```bat
scripts\run-lan-check.cmd --camera-ip 192.168.1.60 --local-address 192.168.1.43
```

Expected JSON fields: `interfaces`, `subnet.sameSubnet24`, `probe.reachable`,
`discovered`, `firewallHint`, `nextExperiment`. Exit code `0` means TCP
`15740` is reachable; `2` means it is not.

4. Pair with a prompt wait (use this instead of `--init` alone — `--init` and
`--mtp-smoke` time out after 5s, which is shorter than the time needed to
accept the on-camera prompt, and the camera then reports Err 11):

```bat
scripts\run-pair.cmd --camera-ip 192.168.1.60 --local-address 192.168.1.43
```

Watch the camera and accept any connection / image-viewing prompt within 90s.
Success returns `ok: true`, `cameraName`, `deviceInfo` (model, firmware,
serial), `liveViewOperations` (which `0x9114/0x9115/0x9116/0x9153` opcodes the
M50 advertises), and `eventPollResponses`. Failure names `failedStep` plus a
`hint`; record those verbatim.

EVF output note: the T100 contract writes output device `8`. DIGIC-8 bodies
(M50) use `0 none / 1 camera / 2 host / 3 cameraAndHost`; use `--evf-output 3`
(and the OBS plugin writes `3`) so the LCD stays live while streaming. An
out-of-range value is ACKed but never streams (`0xa102` forever).

5. Start the bridge only after `--pair` returns `ok: true`:

```bat
scripts\run-bridge.cmd --camera-ip 192.168.1.60 --local-address 192.168.1.43
```

OBS Browser Source:

```text
http://127.0.0.1:8787/live.mjpg
```

Dashboard:

```text
http://127.0.0.1:8787/
```

## 3. Firewall (explain, never silently change)

Rivet never adds firewall rules by itself. If `--lan-check` reports
unreachable, allow:

- Outbound TCP to `<camera-ip>:15740` (PTP/IP command + event channels).
- UDP `239.255.255.250:1900` both directions for SSDP discovery.

Elevated PowerShell example (scoped to the camera IP, Private profile only):

```powershell
New-NetFirewallRule -DisplayName "Rivet Canon PTP/IP" -Direction Outbound -Protocol TCP -RemotePort 15740 -RemoteAddress 192.168.1.60 -Action Allow -Profile Private
```

Re-run `run-lan-check.cmd` after adding the rule. Remove the rule when done
if the camera IP changes.

## 4. Known limitations

- M50 Live View resolution, FPS, EVF bootstrap, and `GetViewFinderData
  (0x9153)` behavior are unknown until hardware-tested. The T100 contract
  (`480x320`, 12–16 FPS) must not be quoted for the M50.
- If `--init` returns an unexpected packet type or `--mtp-smoke` returns
  `0x2019` / `0xa102` persistently, capture the exact response code and stop:
  that is the missing-protocol-step signal, not a retry loop.
- Canon EOS Webcam Utility Pro (USB) in `Downloads` is a separate proprietary
  path using `EDSDK.dll` / `EWCPTPIP.dll` / `EWCUPNPSV.exe`. Rivet does not
  load, modify, or bypass those binaries or any subscription check.
- Router AP isolation / client isolation must be off; otherwise SSDP and
  PTP/IP are blocked at the AP even with correct IPs.
