# Freedea v1.0.1

Maintenance release over v1.0.0. The firmware is unchanged apart from the
version string; this release covers documentation hardening and the new
automated user-manual asset. See the [README](README.md) for the full feature
list and [`docs/USER_MANUAL.md`](docs/USER_MANUAL.md) for the operating guide.

## Changes since v1.0.0

- **User manual PDF is now a release asset** (`freedea-user-manual-1.0.1.pdf`),
  built automatically by CI from `docs/USER_MANUAL.md`.
- **Manual PDF rendering fixed**: the PDF is set in Noto Sans (the font the
  build actually installs) and the manual text is plain ASCII except degree
  signs, so nothing can render as missing-glyph boxes.
- **Flashing & recovery guidance** added to the README and the manual (§14):
  Freedea never updates itself (no OTA, no SD-card flashing — USB reflash
  only), and some marketplace "locked" X4 units have their USB serial link
  fused off and cannot be recovered once flashed. Verify your unit with
  `esptool.py --chip esp32-c3 --port <PORT> chip_id` before flashing.

## Assets

- `freedea-x4-1.0.1.bin` — application image, flash at `0x10000` (the X4's
  stock bootloader and partition table are kept). Or use the browser flasher
  at <https://crosspointreader.com/#flash-tools> (Xteink X4 -> Custom .bin):

  ```sh
  esptool.py --chip esp32-c3 --port <PORT> write-flash 0x10000 freedea-x4-1.0.1.bin
  ```

- `freedea-x4-1.0.1.bin.sha256` — integrity checksum.
- `freedea-user-manual-1.0.1.pdf` — the user manual.

## Highlights

- **AC control** — up to 22 functions (mode/temp/fan/swing through button
  lock), capability-gated per unit; displayed values are AC-acknowledged,
  never optimistic. V2 (no auth) and V3 (token+key) LAN protocol.
- **Multi-network Wi-Fi** — up to 4 stored networks, per-network captive-portal
  detection with an on-device login flow for HTTP sign-in forms (hotel/train
  portals), and a `Freedea-XXXX` provisioning hotspot with QR on first boot.
- **Optional WireGuard tunnel** — with NTP time sync (DHCP option 42, static
  fallback), since WG handshakes fail on unsynced clocks.
- **Button lock** — re-applies your last commanded state if someone (pets,
  kids) changes it on the unit itself; an emulation, as the protocol has no
  hardware lock.
- **E-ink discipline** — value-change-driven redraws, partial refreshes for
  most interactions, a burn-in-aware cadence, and a forced full refresh every
  5 minutes of standing content.

## Measured numbers (`x4_release` build, ESP32-C3)

Unchanged from v1.0.0 (identical firmware apart from the version string):

| Metric | Value |
| --- | --- |
| Static RAM | 87,204 B (26.6 % of 320 KB DRAM) |
| App image | 1,472,391 B flash (22.5 % of partition) |
| Free heap after boot | ≈ 77 KB (largest block ≈ 65 KB) |
| Free-heap floor, idle | ≈ 70–73 KB (incl. connected Wi-Fi + AC polling) |
| Button press → refresh complete | ≈ 0.6–1.1 s |
| — of which panel fast waveform | ≈ 0.57 s (hardware floor) |
| — of which rasterize | ≈ 0.2–0.4 s @ 160 MHz (dropdown stepping: 0) |
| Full refresh | ≈ 2.3 s (periodic / first draw only) |

Heap figures are taken from the instrumented development build; the release
build's static footprint is slightly smaller. Idle-current soak for
WireGuard-on vs -off is still running — figures will follow in later notes if
they motivate a keepalive change.

## Known limitations

- **No self-update:** firmware changes always mean reflashing over USB
  (browser flasher or esptool); there is no OTA path and no SD-card flash.
- **"Locked" X4 units** -- some Alibaba/Taobao stock ships with the USB
  serial link fused off -- can only ever be written once, through the
  vendor's OTA tool. Flashing such a unit is unrecoverable; verify with
  `esptool.py --chip esp32-c3 --port <PORT> chip_id` first.
- Captive-portal login handles **HTTP** sign-in forms; HTTPS-only portals with
  certificate pinning are out of scope.
- WireGuard needs reachable NTP and a route from your Wi-Fi network to the
  tunnel endpoint; guest-network/client-isolation topologies silently drop
  handshakes (the UI reports the failure rather than pretending success).
- E-ink is slow by nature; the periodic full refresh visibly flashes every
  5 minutes by design (burn-in mitigation).
- Telemetry richness (power, energy, humidity, …) depends on the AC model;
  unsupported fields show `no data`.
- Capability-gated extras (iECO, ionizer, self-clean, button-lock emulation,
  …) are verified against a limited set of AC models — please report
  model-specific quirks.

## Building & licensing

`bin/build_docker.sh` (Docker or Podman, no local toolchain) reproduces the
release binary; pins live in [`versions.env`](versions.env) with rationale in
[`DEPENDENCIES.md`](DEPENDENCIES.md). The firmware is MIT-licensed; every
third-party component keeps its own license (in-device **About → Licenses**,
`tools/licenses/`).
