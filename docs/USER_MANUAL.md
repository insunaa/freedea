**Freedea** -- an open-source Wi-Fi controller for Midea air conditioners,
running on the **Xteink X4** (Espressif ESP32-C3): an 800x480 black-and-white
e-ink panel, five buttons, a rechargeable battery, a microSD card slot, and USB-C
charging.

This manual covers everyday use: setting the device up, connecting to Wi-Fi and
to your air conditioner, controlling every exposed feature, and diagnosing
problems.

---

## 1. What Freedea Does

Freedea talks **directly to your Midea air conditioner over your local Wi-Fi
network** -- no cloud account, no vendor servers, no internet required for
control. It shows the live state of the AC on the e-ink panel and gives you a
full remote control through the buttons, including many features the stock
remote buries in app menus.

At a glance:

- **Live dashboard** -- power, mode, target and indoor temperature, fan, swing,
  current power draw, and an optional local weather panel with a 12-hour
  forecast graph.
- **Full control** -- up to 22 control rows (mode, target temperature, fan,
  swing, Eco, Turbo, iECO, outdoor-unit silent, ionizer, 8 °C heating, sleep,
  follow-me, self-clean, breeze away, breezeless, jet cool, power limit, wind
  around, fresh air, display, beeper sound, button lock). Which rows appear
  depends on what *your* air conditioner reports it supports.
- **Detailed engineering data** -- compressor frequency and current, coil and
  outdoor temperatures, fan RPM, humidity, and energy consumption.
- **Multi-network Wi-Fi** -- up to 4 saved networks, automatic login to
  captive portals (train/bus/hotel Wi-Fi), and QR-code provisioning so you
  never have to type a password on the device.
- **WireGuard VPN client** -- reach the device (and the AC behind it) from
  outside your home network.
- **Battery friendly** -- Wi-Fi modem sleep, CPU downclocking when idle, and a
  true power-off (long-press power).
- **E-ink disciplined** -- the screen only refreshes when something actually
  changed; a periodic deep refresh keeps the panel free of ghosting.

The interface adapts automatically to how you hold the device (portrait or
landscape, either way round).

---

## 2. Getting to Know the Device

### 2.1 Buttons

The X4 has three **seesaw bars** -- each bar is two buttons, one on each half:

| Bar | Left/Top half | Right/Bottom half |
|---|---|---|
| Bottom-left bar | **Back** | **Confirm** |
| Bottom-right bar | **Left** | **Right** |
| Side bar | **Up** | **Down** |

Chips drawn at the bottom edge of the screen always show what the buttons do on
the current screen: `Back`, `Open`, `Add`, `Remove`, `Save`, `-`/`+`, and so
on. A chip sits directly above the half-bar that performs that action. On
scrolling screens (Home, Details, lists) the bottom-right bar doubles as
Up/Down.

### 2.2 Power button

| Gesture | Effect |
|---|---|
| **Short press** | Full "deep" screen refresh -- clears any ghosting. |
| **Hold >= 3 s** | Power off. The screen freezes on a "Powering off" image (e-ink holds it with no power at all). The device is fully off on battery; press the power button (or plug in USB) to turn it back on. |

### 2.3 The status strip

Every screen shows a small status area at the top:

- **Battery percentage** at the right. The X4 reports battery by voltage, so
  the figure is approximate, and it stays visible for weeks after power-off.
- **Wi-Fi banner** -- while Wi-Fi is connecting or disconnected, an inverted
  banner appears on every screen. It disappears once the link is up.
- **Toasts** -- temporary inverted strips at the bottom announce errors and
  events (e.g. "Portal login complete", "Lock: change reverted") for ~15 s.

### 2.4 E-ink behavior

The screen only redraws when a value actually changes, so a picture can sit on
it indefinitely. Occasional ghosting (faint outlines of old content) is normal
for e-ink and is cleaned by the automatic deep refresh or by a short power-button
press. If a value changes while you are on another screen, that screen is
already correct the moment you open it.

### 2.5 microSD card

**Keep a microSD card inserted.** All settings, Wi-Fi credentials, AC device
records and caches live in an encrypted form on the card (`/.freedea/`). The
card can also be read in a PC for the hand-editable import files described in
sec. 12.

---

## 3. First-Time Setup

When Freedea starts with **no Wi-Fi network configured**, it boots straight
into **provisioning mode**:

1. The screen shows a **QR code**, a network name `Freedea-XXXX`, a 12-character
   password, and the address `http://192.168.4.1`.
2. Scan the QR code with your phone (it is a standard Wi-Fi Easy Connect code --
   the phone joins automatically), or join `Freedea-XXXX` manually with the
   password on screen.
3. Open **http://192.168.4.1** in a browser. The second, smaller QR code on the
   screen encodes this URL.
4. Enter your home network's name and password in the form and **Save**.
5. The device saves the credentials and **reboots automatically**, then connects
   to your network. The provisioning access point never comes up again unless
   you ask for it (Settings -> Hotspot).

> **Note on reboots:** every setting saved through the portal that affects the
> radios (Wi-Fi credentials, connecting a network) is committed by rebooting
> the device. This is by design and completes in a few seconds.

### Re-provisioning later (lost network, wrong password)

If the device can't reach your network and you can't reach its settings, go to
**Settings -> Hotspot** and confirm. The device reboots into the provisioning
mode above. This recovery path always works -- even if the stored credentials
are broken -- and if you walk away without saving, the next boot is a normal one.

### Adding the air conditioner

Your AC must be on the **same LAN** as the device (see the client-isolation
note in sec. 13).

1. Go to **Settings -> Devices**. The row shows how many ACs are saved.
2. Confirm to start a **scan** (a few seconds; Back cancels). Any Midea device
   on the LAN appears with its name, protocol version and IP.
3. Select a result and Confirm to **add** it. Already-saved devices are marked
   ` saved`.

**Protocol versions:**

- **V2 devices** start working right away after adding (they need no secret).
- **V3 devices** (most current Midea/PortaSplit units) need a **token and key**
  before Freedea can control them. Freedea adds the record immediately but shows
  `V3` in the list; enter the credentials through the web portal (sec. 9.4), then
  the device reboots and connects.

#### Getting a V3 token and key

The token (128 hex characters) and key (32 hex characters) are per-device LAN
credentials issued through the **Midea cloud**. Freedea does not talk to the
cloud itself; obtaining these two values is a one-time setup step done outside
Freedea. Follow the token/key extraction instructions in the
[midea-msmart](https://github.com/mill1000/midea-msmart) documentation (the
project whose protocol Freedea implements), then paste both values into the
device form in Freedea's web portal.

You only need to do this once per air conditioner; both values survive reboots.

---

## 4. Wi-Fi Networks

Freedea stores up to **4 Wi-Fi networks**. One is the **active** network; the
device connects to it at every boot.

- **Settings -> WiFi** shows the current link state (e.g. `Connected 192.0.2.42`)
  and opens the **network list**.
- The **network list** shows each saved network. Hints tell you what's what:
  `active` marks the connected profile; `internet ok` or `login needed` show the
  result of the captive-portal check for that network. Move the cursor with
  Up/Down and press **Confirm** on another network to switch to it immediately.
- Networks are **added, edited, and deleted in the web portal** (sec. 9.4) --
  Freedea has no keyboard. Editing a network's password or SSID resets its
  captive-portal check.

### Captive portals (train, bus, and hotel Wi-Fi)

Networks that intercept your traffic with a "accept terms -> submit" login page
are supported. On each first connect to a network Freedea quietly checks
whether the network is open or intercepting, and remembers the verdict per
network.

When a network is known to have a login page, Freedea shows a prompt after
connecting:

- **Confirm** -- Freedea fetches the login page, fills it in automatically
  (ticks checkboxes, keeps hidden default values, presses the submit button)
  and reports **Portal login complete** or **Could not join network**.
- **Back** -- stay connected without attempting the login; the prompt does not
  repeat until the next time the link comes up.

Limits: only HTTP login pages work (an HTTPS portal is reported as
unsupported), and forms that require *typing* (name, SMS code, password) cannot
be filled -- the attempt simply fails gracefully.

---

## 5. Home Screen and Navigation

The root **Home** menu offers four entries:

| Entry | What it is |
|---|---|
| **Dashboard** | Live view of the AC + weather. |
| **Control** | All control rows. |
| **Settings** | Wi-Fi, devices, weather, hotspot, web portal, WireGuard. |
| **About** | Version, free memory, open-source licenses. |

Navigate with the side Up/Down bar (or the bottom-right bar), select with
Confirm, and leave any screen with Back.

---

## 6. Dashboard

The Dashboard is the live view. It shows:

- **Power, Mode, Target and Indoor temperature, Fan, Swing** -- updated from the
  AC automatically (typically within ~5 s of a change made on the AC's own
  remote). Fields show `no data` until the first state arrives.
- **Draw** -- the outdoor unit's current power consumption in watts.
- **Weather** (if enabled, sec. 9.3) -- a line like `Berlin 21.3°C Cloudy 63%` plus
  a 12-hour temperature sparkline with min/max labels and hour marks.
- **Stale indicator** -- if the AC stops answering, a centered
  `stale - last update m:ss ago` line appears; the last known values stay on
  screen.

Press **Confirm** (chip: `Details`) to open the **Details** screen: a
scrollable engineering readout -- compressor frequency (actual and target),
compressor current and voltage, indoor/outdoor coil temperatures, outdoor
temperature, discharge temperature, indoor and outdoor fan RPM, humidity,
outdoor-unit power, and energy (live watts / this run / total kWh). Fields the
AC does not report show `no data`. While open, the screen refreshes every ~3
seconds.

> Polling is screen-aware: the Dashboard and Details screens actively query
> the AC; the Home menu and Settings screens do not generate AC traffic at all.

---

## 7. Control Screen

The Control screen is a list of rows. The side bar (or the right bar) moves
between rows; **Left/Right** (`-`/`+` chips) change a value directly;
**Confirm** opens a dropdown list of options for that row (or triggers an
action row). Rows you do not see are features your AC did not report supporting.

| Row | Values | Notes |
|---|---|---|
| **Mode** | Auto, Cool, Heat, Dry, Fan | Only modes your AC supports appear. |
| **Target** | °C | Left/Right steps in **0.5 °C**; Confirm opens a whole-degree dropdown. Range is bounded by the AC. |
| **Fan** | Auto, Silent, Low, Medium, High (, Max) | "Max" appears only on units with custom fan-speed control. |
| **Swing** | Off, Vertical, Horizontal, Both | Only directions the AC supports; row hidden if none. |
| **Eco** | On / Off | The classic eco bit. |
| **Turbo** | On / Off | |
| **iECO** | On / Off | "Intelligent eco" -- a separate feature from Eco. Appears only when the AC reports support. |
| **Out silent** | On / Off | Silences the **outdoor unit**. Only on units that report support. |
| **Ionizer** | On / Off | Anion / air-purifier function. |
| **8C heat** | On / Off | Freeze protection: keeps the room from dropping below ~8 °C. |
| **Sleep** | On / Off | Sleep-mode bit (no sleep-curve editing). |
| **Follow me** | On / Off | Makes the AC follow the temperature sensor of *a remote control* -- Freedea supplies no temperature, so this is informational. |
| **Self clean** | `Start` / `Running...` | A one-shot trigger, not a toggle. Confirm starts the cycle; the row shows "Running..." until the AC finishes. |
| **Breeze away** | On / Off | |
| **Breezeless** | On / Off | |
| **Jet cool** | On / Off | |
| **Power limit** | Off, 50 %, 75 % **or** Off, Level 1-5 | Limits compressor power; the list follows what the AC reports. |
| **Wind around** | Off, Up, Down | Avoids blowing directly at you. |
| **Fresh air** | Off, Low, Medium, High, Boost | Only on units with a fresh-air supply. |
| **Display** | On / Off | Toggles the AC's own front-panel display. |
| **Sound** | On / Off | The AC's **beeper**. Off silences both the AC's own buttons and the chirp that accompanied Freedea's commands. |
| **Button lock** | On / Off | See sec. 8. |

**Values shown are the AC's acknowledged truth.** After you change something,
the new value appears once the AC confirms it (usually ~1 s) -- if the AC
refuses or someone else changed it in the meantime, the screen follows the AC,
not your edit.

With many rows visible the list scrolls: rows shrink to fit when they can, and
a `row/total` counter appears when some rows are off-screen.

---

## 8. Button Lock (anti-pet mode)

The AC's own buttons can be pressed by pets (or children), silently changing
your carefully set temperature. The Midea protocol has no true button lock --
so Freedea provides it by **monitoring and overriding**:

- Turn **Button lock** On in Control. The current AC state is frozen as the
  lock target.
- Whenever the AC state changes in a way Freedea did not command (a unit button
  press, the IR remote, or another app), Freedea sends the locked state back --
  the unwanted change disappears within a few seconds.
- **Your edits still work**: changing a value on the Control screen moves the
  lock target to the new state.
- **Unlock before intentionally using the remote**, or the remote will appear
  broken.
- What is *not* locked: property-based features (iECO, self-clean, fresh air,
  ...) -- buttons rarely touch those and they don't travel on state updates.
- Cost: while locked, Freedea keeps the AC session active and polls every ~5 s
  even when no screen is open (a small extra battery drain -- you opt into this
  by enabling the lock; a toast reminds you).
- If the AC refuses the override three times in a row, Freedea gives up with a
  **"Button lock stuck"** toast rather than hammering the AC.

The lock survives reboots and re-adopts whatever state the AC is in.

---

## 9. Settings

### 9.1 WiFi

Shows the link state and opens the network list (sec. 4).

### 9.2 Devices

Saved air conditioners and discovery (sec. 3). Selecting a saved device and
pressing Confirm lets you **Remove** it (confirmed, then the device reboots).

### 9.3 Weather

Enables the Dashboard weather panel. Freedea uses **Open-Meteo** -- free, no
account, no API key. Setup is done in the web portal (sec. 9.4): a display name
plus latitude/longitude. Once enabled, the device fetches current conditions
and a 12-hour forecast at boot and every 30 minutes.

*Privacy note:* the fetch is plain HTTP (no TLS) and contains your coordinates.
Nothing else leaves the device for weather, and the panel works without any
account.

### 9.4 Web portal (on the home network)

The Settings -> **Web portal** row starts the configuration web server on the
device's LAN IP (shown on the row, e.g. `http://192.0.2.42`). Open that
address from any computer or phone on your network.

- The portal listens **only while the row is on**. It closes automatically
  after **15 minutes without a request**, and it is always closed after a
  reboot -- this keeps port 80 shut by default.
- The provisioning portal (reached via Settings -> Hotspot) is separate and
  always stays on while in provisioning mode.

The portal offers:

| Section | What you can do |
|---|---|
| **Wi-Fi networks** | Add a network (first free slot), edit (blank password keeps the stored one), remove, and **Connect** (make active). Changes that move the active network reboot the device; editing *inactive* profiles saves without a reboot. |
| **Midea devices** | Add a device by hand (id, optional name and IP, token + key for V3), and **Remove** saved devices. Secrets are never shown back in the browser -- blank fields keep stored values. |
| **Weather** | Enable/disable, location name, latitude, longitude. |
| **WireGuard** | See sec. 10. |

### 9.5 Hotspot

Reboots into provisioning mode (sec. 3) -- the recovery path for forgotten or
broken Wi-Fi credentials.

### 9.6 WireGuard row

Shows tunnel state (`off`, `incomplete`, `wifi down`, `waiting`,
`connecting...`, `handshaking`, `up <ip>`, `failed`) and **toggles the tunnel
on/off** with Confirm. See sec. 10.

### 9.7 About

Version number, free memory, and a **Licenses** entry: a scrollable viewer with
the full license text of every open-source component (Freedea's own MIT
license, ESP-IDF, Arduino, FreeInk SDK, qrcodegen, mbedTLS, lwIP, WireGuard,
and the Lucide icon set).

---

## 10. Remote Access with WireGuard

Freedea includes a **WireGuard VPN client**, letting you reach the device -- and
the AC behind it -- from outside your network, or letting the device reach your
home network from an isolated guest Wi-Fi.

**Setup:**

1. On your WireGuard **server**, create a peer for the device and note the
   server's endpoint (host + UDP port) and the peer's allowed/source IPs.
2. Generate a key pair for the device (on any machine with `wg` tools):
   `wg genkey` -> private key; `wg pubkey` -> public key (add the peer with this).
3. In Freedea's **web portal -> WireGuard**, enter: enabled, endpoint host and
   port, the device's **private** key, the server's (peer) **public** key, the
   device's tunnel IP, and a keepalive seconds value. Keys are never displayed
   back once saved; blank fields keep the stored values.
4. Toggle it on from **Settings -> WireGuard**. The row shows `up <tunnel IP>`
   once the handshake succeeds (usually ~1 s).

**Things to know:**

- While the tunnel is up it becomes the device's **default route**
  (AllowedIPs `0.0.0.0/0`): weather fetches and AC traffic ride the tunnel, so
  your WG server must route/NAT them (internet) and route the AC's subnet
  (LAN access). Plan the server's `AllowedIPs`/forwarding accordingly.
- **Time matters:** WireGuard handshakes embed a timestamp and are silently
  dropped when it is wrong. The device has no battery clock, so it
  automatically NTP-syncs its clock before every tunnel bring-up (via your
  network's DHCP time server, falling back to Cloudflare). Handshakes failing
  with *no trace on the server* are almost always this.
- **Keepalive** (default 25 s) wakes the Wi-Fi radio periodically. If you want
  maximum battery life, raise the interval or leave WireGuard off until you
  need it -- the toggle is in Settings.
- A failed bring-up backs off (30 s ... 15 min) and retries on its own; pressing
  Confirm on the row kicks it immediately.

---

## 11. Battery and Power

- The battery percentage in the status strip updates every minute.
- Freedea is idle-efficient by design: the Wi-Fi radio sleeps between beacons,
  the CPU drops to half clock after 10 s of no button presses (back to full
  speed on the next press), and the e-ink panel consumes nothing while an
  image is held.
- **Long-press power (>= 3 s) turns the device fully off** -- for storage or
  maximum runtime. The last image stays on the panel.
- Features that cost battery, roughly in order: WireGuard keepalive, button
  lock's constant polling, weather fetches every 30 min, frequent manual
  screen refreshes.

---

## 12. Storage, Data and Privacy

**On the microSD card** (`/.freedea/`):

| File | Contents |
|---|---|
| `settings.bin` (+`.bak`) | All settings incl. Wi-Fi passwords and WireGuard keys -- **AES-256 encrypted** with a key derived from this device's Wi-Fi MAC. A foreign SD card cannot read or use them; a corrupt copy self-repairs from the backup. |
| `devices.bin` (+`.bak`) | Saved AC records (ids, IPs, V3 token/key) -- same encryption. |
| `caps.bin` | Your AC's feature capabilities (not sensitive). |
| `provision.flag` | Temporary marker set by Settings -> Hotspot; consumed at boot. |

**Recovery by hand:** drop a plaintext `settings.json` / `device.json` on the
card and delete the matching `.bin` pair(s); on the next boot Freedea imports
them and re-encrypts. (This is also the migration path from older versions.)
The byte-level format of every file above -- including the JSON schemas -- is
specified in [`file-formats.md`](file-formats.md).

**What leaves the device:**

- The Midea protocol -- always **local LAN only** (or through *your* WireGuard
  tunnel). No Midea cloud is ever contacted.
- Open-Meteo (HTTP) for weather: your coordinates only.
- The captive-portal probe (a tiny request to `clients3.google.com`) and NTP
  time sync, each only when relevant.
- Nothing is telemetryed to the Freedea project itself.

---

## 13. Troubleshooting

**Won't connect to Wi-Fi / I changed my password.**
Settings -> Hotspot -> re-provision (sec. 3). This always works.

**Dashboard shows `no data` or a `stale` line.**
The AC is unreachable. Check the Wi-Fi banner (network down?), then that the
AC still has power and is on the same network. Freedea reconnects by itself
(backoff between 5 s and 60 s).

**Scan finds no devices, or the AC never connects, although both are on Wi-Fi.**
The classic cause is **client isolation** on your router (especially guest
networks): wireless clients cannot talk to each other. Disable AP client
isolation, or put Freedea and the AC on the same non-isolated segment.

**V3 device refuses to authenticate.**
Wrong token/key. Re-extract them from the Midea cloud (see sec. 3) and re-enter
them in the portal; the device record is matched by its id.

**WireGuard says `failed` / never `up`.**
Check in order: endpoint host/port and keys; server has the device's *public*
key as a peer with the right allowed IP; UDP reaches the server; and -- most
subtly -- time: the device NTP-syncs automatically before bring-up, so if your
network blocks UDP/123 and the tunnel never comes up, allow NTP or try a
different network. The row auto-retries on a backoff ladder; Confirm kicks it.

**"Button lock stuck" toast.**
The AC rejected three overrides in a row. Move the value once from Freedea, or
toggle the lock off/on.

**Captive portal login fails ("Could not join network").**
Portals that demand typed credentials (SMS codes, room numbers) or that use
HTTPS cannot be automated. Join such networks only if you can complete the
login another way, or use a network without a portal.

**Screen looks faded or has ghost outlines.**
Short-press the power button for a full deep refresh. Freedea also performs
this automatically every 5 minutes of stillness and periodically during use.

**The web portal doesn't open.**
It is closed by default and closes on reboot and after 15 idle minutes --
toggle Settings -> Web portal on first. During a provisioning boot the portal
only lives on the hotspot at `192.168.4.1`.

**Device unresponsive.**
Hold power >= 3 s to force off, then power on. (All state is on the SD card; a
forced power-off loses nothing.) Corrupted settings self-repair from their
backup; if the device still misbehaves, re-import a plaintext `settings.json`
as described in sec. 12.

---

## 14. Building the Firmware (for developers)

```bash
git clone --recursive https://github.com/insunaa/freedea   # includes the FreeInk SDK submodule
cd freedea
pio run -e x4            # build
pio run -e x4 -t upload  # flash over USB
```

The protocol layer (`lib/MideaAC`) and the settings/config layers run their
golden-vector host test suites on any Linux machine
(`lib/MideaAC/test/host/run.sh`, `lib/Settings/test/host/run.sh`).

### Flashing and updates

The **first install** of Freedea needs a computer and a USB cable: either
the browser flasher at <https://crosspointreader.com/#flash-tools> (choose
Xteink X4, then "Custom .bin") or esptool. Back up the stock firmware
before the first flash:

```bash
esptool.py --chip esp32-c3 read-flash 0 0x1000000 stock.bin
```

**Some X4 units are "locked" and cannot be recovered once flashed.** A
portion of units sold through marketplaces such as Alibaba or Taobao ship
with the USB serial link fused off: esptool cannot reach them, and they can
be written exactly once through the vendor's OTA tool. Flashing Freedea
onto such a unit is irreversible; there is no path back to the factory
reader software or to any later firmware. Before flashing anything, verify
that esptool can reach the unit:

```bash
esptool.py --chip esp32-c3 --port <PORT> chip_id
```

If that command does not respond, the unit is locked and must not be
flashed.

### Updating from the microSD card

Once Freedea v1.1.0 or later is running, later releases can be installed
from the microSD card without reflashing over USB:

1. Take the microSD card out of the device and put it in a computer (a
   card reader works; the X4 does not expose its SD card over USB, and the
   firmware has no download feature of its own).
2. Download `freedea-x4-<version>.bin` from the GitHub releases page, copy
   it to the **root** of the card, rename it to `update.bin`, and put the
   card back into the device.
3. Turn the device fully off (hold power >= 3 s). Then hold **Back** +
   **Up**, press power to turn the device on, and keep Back + Up held for
   a few seconds.
4. Wait. The screen stays dark for up to a minute -- that is the flash in
   progress (progress lines only go to the serial port, if one is
   attached). The device then reboots into the new firmware on its own.

The new image is validated (magic, chip, checksums) before anything is
written, and goes into a second app slot, so the currently installed
firmware stays untouched. A bad or wrong `update.bin` is rejected and the
device just boots normally. On success the file is renamed to
`update.bin.flashed` so the update cannot retrigger by accident.

Holding **Back** + **Up** at power-on with **no `update.bin` on the card**
boots the previously installed firmware instead -- the escape hatch if an
SD-updated image misbehaves. A USB reflash always installs into the first
slot and boots that by default.

There is still no update over Wi-Fi: the SD card path needs no network,
and the radio is never involved in flashing.

---

## 15. Credits and License

Freedea is licensed under the **MIT license** (see `LICENSE` in the repository
and the on-device Licenses viewer).

It stands on the work of others: the [FreeInk SDK](https://freeink.org) for the
Xteink X4, the [midea-msmart](https://github.com/mill1000/midea-msmart) project
whose Python sources served as the protocol reference, [qrcodegen](https://github.com/nayuki/QR-Code-generator)
by Nayuki, WireGuard (BSD-3, `wireguard-lwip`/`WireGuard-ESP32-Arduino`),
Espressif's ESP-IDF and the Arduino core, mbedTLS, lwIP, and [Lucide](https://lucide.dev)
for the interface icons (ISC). Full texts: **About -> Licenses** on the device.
