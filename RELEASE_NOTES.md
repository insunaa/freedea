# Freedea v1.1.0

Feature release: **firmware updates from the microSD card**, no computer
required. Copy a release `.bin` to the card root as `update.bin`, power on
while holding **Back + Up**, and the device validates and flashes it into
the spare OTA slot and reboots into it. Details in the
[user manual](docs/USER_MANUAL.md) (§14) and the [README](README.md).

## Changes since v1.0.1

- **SD-card self-update** (FreeInk SDK `RecoveryBoot`): with `update.bin` at
  the SD root, holding **Back + Up** through a power-on validates the image
  (magic, chip ID, segment table, XOR checksum, SHA256) and streams it into
  the other app slot — the running firmware is untouched until the new image
  is fully written and verified, so a bad or wrong file simply boots the
  current firmware. On success the file is renamed to `update.bin.flashed`
  and the device reboots into the new firmware. Holding **Back + Up** with
  no `update.bin` present boots the previously installed image instead (the
  escape hatch if an SD-updated image misbehaves). The panel stays dark
  during the flash (about a minute; serial shows progress lines). Note the
  copy step itself needs the card in a computer: the X4 does not expose its
  SD card over USB and there is no in-app download.
- **Partition table matches the stock dual-OTA X4 layout**: a second app
  slot (`app1` @ `0x650000`) plus the stock spiffs/coredump regions, so
  app-only flashing at `0x10000` keeps working and the SD update has a
  spare slot to install into.
- **FreeInk SDK bumped** `e0fcdb1` -> `2cca22f` (adds the `RecoveryBoot`
  library; also upstream display/input/SD-card fixes since our previous
  pin).

## Assets

- `freedea-x4-1.1.0.bin` — application image. Existing installs are upgraded
  in place by this version over USB once; from then on the SD-card path
  above carries updates. For a first install, flash at `0x10000` (the X4's
  stock bootloader and partition table are kept). Or use the browser flasher
  at <https://crosspointreader.com/#flash-tools> (Xteink X4 -> Custom .bin):

  ```sh
  esptool.py --chip esp32-c3 --port <PORT> write-flash 0x10000 freedea-x4-1.1.0.bin
  ```

- `freedea-x4-1.1.0.bin.sha256` — integrity checksum.
- `freedea-user-manual-1.1.0.pdf` — the user manual.

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

| Metric | Value |
| --- | --- |
| Static RAM | 87,260 B (26.6 % of 320 KB DRAM) |
| App image | 1,484,033 B flash (22.6 % of partition) |
| Free heap after boot | ≈ 77 KB (largest block ≈ 65 KB) |
| Free-heap floor, idle | ≈ 70–73 KB (incl. connected Wi-Fi + AC polling) |
| Button press → refresh complete | ≈ 0.6–1.1 s |
| — of which panel fast waveform | ≈ 0.57 s (hardware floor) |
| — of which rasterize | ≈ 0.2–0.4 s @ 160 MHz (dropdown stepping: 0) |
| Full refresh | ≈ 2.3 s (periodic / first draw only) |

Static RAM/app size are from this build; the SD-update hatch adds ~56 B of
static RAM (heap figures from the v1.0.x soak, expected unchanged — the
flasher runs once at boot before the app allocates anything). Idle-current
soak for WireGuard-on vs -off is still running — figures will follow in
later notes if they motivate a keepalive change.

## Known limitations

- **No update over Wi-Fi:** the SD-card path needs no network, and the
  radio is never involved in flashing. Recovery from a broken install still
  means USB (browser flasher or esptool).
- **SD-card updates require Freedea v1.1.0 or later** — there is no
  chicken-or-egg path; the first v1.1.0 install itself goes over USB.
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
