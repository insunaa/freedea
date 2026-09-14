# Freedea

Open-source firmware that turns an **Xteink X4** e-ink reader into a dedicated
wall controller for **Midea air conditioners** — fully on your LAN, no vendor
cloud required.

The firmware runs on the X4's ESP32-C3 (RISC-V, 380 KB RAM, no PSRAM) and
drives its 800×480 monochrome e-ink panel. Air-conditioner communication uses
the local Midea LAN protocol (V2/V3), implemented from the reverse-engineered
specifications of the
[midea-msmart](https://github.com/mill1000/midea-msmart) project.

*Not affiliated with Midea,美的, or Xteink. "Midea" is a trademark of its
owner; this project speaks the air conditioner's documented-on-LAN protocol.*

## Features

- **Dashboard** — live target/indoor temperature, live power draw, and an
  optional weather panel (Open-Meteo, coordinates configurable); a Details
  screen exposes compressor frequency, coil/outdoor temperatures, humidity,
  voltage, RPM and energy counters when the AC reports them.
- **Control** — up to 22 functions (mode, target, fan, swing, Eco, Turbo,
  iECO, outdoor-unit silent, ionizer, 8 °C heat, sleep, follow-me, self-clean,
  breeze away/less, jet cool, power limit, wind-around, fresh air, display,
  buzzer, button lock). Rows appear only when the connected unit's
  capability report says it supports them; values shown are acknowledged by
  the AC, not optimistic.
- **Multi-device** — LAN discovery of Midea ACs; V2 (no auth) and V3 (LAN
  token + key, entered through the web portal) supported.
- **Button lock** — the AC's own buttons can be spammed by pets/kids; Freedea
  watches the unit and re-applies your last commanded state when someone
  changes it outside the app (an emulation: the protocol has no hardware
  lock).
- **Wi-Fi done properly** — up to 4 stored networks with per-network captive
  portal detection and an on-device login flow for hotel/train portals
  (HTTP forms), plus a provisioning hotspot with QR code for first setup.
- **WireGuard** — optional outbound tunnel for networks that require it
  (clock auto-syncs via NTP, since WG drops handshakes with unsynced
  timestamps).
- **Web portal** — configure Wi-Fi, devices, credentials and WireGuard from
  any browser on the LAN.
- Deliberately e-ink-friendly: refresh discipline to avoid burn-in, periodic
  full refreshes, and slow-refresh-aware UI.

## Hardware

| Device | Xteink X4 (ESP32-C3, 16 MB flash) |
| --- | --- |
| Display | 800×480 SSD1677 e-ink, single 48 KB framebuffer |
| Storage | microSD (all persistent state lives here, under `/.freedea/`) |
| Radio | Wi-Fi 4 (2.4 GHz), optional WireGuard on top |

## Try it

> [!CAUTION]
> **Freedea has no way to update itself: no OTA updates and no flashing from
> the SD card.** Every firmware change — including future releases and any
> recovery — requires connecting the X4 to a computer and reflashing (browser
> tool or esptool, below). Back up the X4's stock firmware *before* flashing
> (e.g. `esptool.py --chip esp32-c3 read-flash 0 0x1000000 stock.bin`).
>
> **Some X4 units are "locked" and cannot be recovered once flashed.** A
> fraction of units sold through marketplaces such as Alibaba or Taobao ship
> with the USB serial link fused off: esptool cannot reach them, and they can
> be written exactly once, through the vendor's OTA tool. On such a device
> flashing Freedea is irreversible — there is no path back to the factory
> reader software or to any future firmware, including later Freedea
> versions. Before flashing anything, verify esptool can talk to your unit
> (e.g. `esptool.py --chip esp32-c3 --port <PORT> chip_id` responds); if it
> does not, the unit is locked and must not be flashed. (The browser flasher
> below rides the same serial link, so it cannot reach locked units either.)

Release assets contain a prebuilt `freedea-x4-<version>.bin`.

**Flash from the browser** (recommended, no toolchain): open the CrossPoint
Reader [web flasher](https://crosspointreader.com/#flash-tools) in Chrome,
Edge or Firefox, choose **Xteink X4**, select **Custom .bin**, upload
`freedea-x4-<version>.bin`, and press **Flash**.

**Flash from the command line** with
[esptool](https://github.com/esptool/esptool):

```sh
esptool.py --chip esp32-c3 --port <PORT> write-flash 0x10000 freedea-x4-<version>.bin
```

This writes the application only; the X4's stock bootloader and the standard
C3 partition table (`nvs` @ `0x9000`, `otadata` @ `0xe000`) match Freedea's
layout. If in doubt, flash the complete set from source with
`pio run -e x4_release -t upload`.

On first boot the device opens a `Freedea-XXXX` provisioning hotspot (XXXX =
the low MAC bytes); connect
to it and follow the web setup. The full operating guide lives in
[`docs/USER_MANUAL.md`](docs/USER_MANUAL.md)
(`bin/build_manual_pdf.sh` renders it to PDF).

## Build from source

No local toolchain needed (Docker or Podman):

```sh
bin/build_docker.sh            # -> dist/freedea-x4-<version>.bin (+ sha256)
```

Windows: `.\bin\build_docker.ps1`. Or use the devcontainer (`.devcontainer/`,
see [`docs/devcontainer-setup.md`](docs/devcontainer-setup.md)) and run
`pio run -e x4` (dev build, verbose) / `pio run -e x4_release`. All dependency
pins live in [`versions.env`](versions.env) with rationale in
[`DEPENDENCIES.md`](DEPENDENCIES.md).

Host unit tests (no hardware): `sh lib/<name>/test/host/run.sh` for each of
the seven `lib/*/test/host` suites.

## Configuration

- Build-time pins: [`versions.env`](versions.env)
- Device settings live on the SD card under `/.freedea/` (encrypted binary
  stores with plaintext JSON import/recovery files), edited through the UI or
  the web portal — never by rebuilding firmware. Formats:
  [`docs/file-formats.md`](docs/file-formats.md).
- PlatformIO environments: `x4` (development, debug diagnostics on) and
  `x4_release` (shipping build); flashing is documented in
  `docs/devcontainer-setup.md` §7.

## Privacy

AC control is entirely LAN-local. Two features touch the network beyond your
LAN, both opt-in: the optional weather panel fetches
[Open-Meteo](https://open-meteo.com) over plain HTTP (no account, no API key),
and the optional WireGuard tunnel sends traffic to your own endpoint. V3
tokens/keys are used only against the AC itself.

## Known limitations

- E-ink refreshes are slow by nature (≈1–2 s full updates); the UI batches
  repaints and follows a burn-in-aware refresh cadence.
- Captive-portal login handles HTTP sign-in forms; HTTPS-only portals with
  certificate pinning are out of scope.
- WireGuard needs real wall-clock time; a device without a reachable NTP
  server cannot complete a handshake (it tries DHCP option 42 first, then
  Cloudflare).
- Guest network / client-isolation Wi-Fi topologies may block the LAN or
  WireGuard paths; the UI reports failures instead of pretending success.
- Some telemetry (power, energy, humidity) depends on what your AC model
  reports — missing fields show as `no data`.

## Licensing

The firmware is MIT-licensed ([`LICENSE`](LICENSE)). Third-party components
(the vendored FreeInk SDK, WireGuard port, mbedTLS, msmart-derived protocol
knowledge, Lucide icons, …) keep their own licenses; see the in-device
**About → Licenses** viewer, the `ATTRIBUTION.md`/`LICENSE` files beside
each component and the license texts staged under [`tools/licenses/`](tools/licenses/).
[`DEPENDENCIES.md`](DEPENDENCIES.md) lists every build and regeneration
dependency.
