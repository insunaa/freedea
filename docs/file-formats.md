# Freedea — SD File Formats (`/.freedea/`)

All persistent state lives on the microSD card under `/.freedea/` (created
automatically at boot). This is the byte-level reference; the user-facing
description is [`USER_MANUAL.md`](USER_MANUAL.md) §12.

| File | Content | Protection |
| --- | --- | --- |
| `settings.bin` + `settings.bak` | All settings (Wi-Fi profiles, weather, WireGuard, AC flags) | Vault envelope (encrypted) |
| `devices.bin` + `devices.bak` | AC records incl. V3 token/key | Vault envelope (encrypted) |
| `settings.json` | Settings import/recovery surface | plaintext, transient |
| `device.json` | Hand-editable device import | plaintext, transient |
| `caps.bin` | Capability cache for the current AC | plaintext, CRC-only |
| `provision.flag` | One-shot "enter provisioning on next boot" marker | n/a |

Conventions for every binary file: **little-endian**, fixed-size payloads, and
all multi-byte fields copied byte-wise (never pointer-cast — the ESP32-C3
faults on unaligned loads).

## A/B slots and load order

`settings` and `devices` are written as a **pair**: `X.bin` (primary) and
`X.bak` (backup) in the same save operation. At boot the primary is opened
first; if it is missing/corrupt and the backup validates, the firmware repairs
the primary from the backup. A slot that fails *CRC* is corrupt; a slot that
decrypts to an invalid payload is treated as a **foreign card** (its key came
from another device) and never triggers repair.

## Vault envelope (`settings.bin`, `devices.bin`)

32-byte header + ciphertext (implementation: `lib/Vault/`):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `'F','D','V','1'` |
| 4 | 1 | kind: `1` = settings, `2` = devices |
| 5 | 1 | envelope format version (currently `1`) |
| 6 | 1 | key source: `1` = MAC-derived (reserved: `2` = EFuse HMAC) |
| 7 | 1 | reserved (0) |
| 8 | 2 | `payloadLen` — real payload length inside the padded ciphertext |
| 10 | 2 | reserved (0) |
| 12 | 4 | CRC-32/ISO-HDLC over bytes `[16, end)` (`iv || ciphertext`) |
| 16 | 16 | IV, fresh random per write |
| 32 | … | AES-256-CBC ciphertext, payload zero-padded to a 16-byte block |

The store key is `sha256("freedea:vault:v1" || mac)` over the chip's EFuse
Wi-Fi STA MAC — it never touches the card. Threat model: a lost SD card
cannot be read; a determined attacker holding the *device* can re-derive the
key (open firmware, JTAG), which is out of scope and the reason for the
reserved EFuse-HMAC key source.

## Settings payload (kind `1`)

Versioned fixed-size snapshots; **length and version are bound 1:1**, and the
reader accepts every legacy length (migrating) while writing only the current
one (`kSchemaVersion` 5). Field sequences (`lib/Settings/include/Settings.h`
is authoritative):

Current **v5, 610 B**:

| Offset | Layout |
| --- | --- |
| 0 | version u16 LE (=5), activeNetwork u8, reserved u8 |
| 4 | 4 × Wi-Fi profile slots of 99 B: `ssid[33]`, `password[65]`, `portalState` u8 (`0` unknown / `1` open / `2` captive portal found) |
| 400 | weather block 26 B: `enabled` u8, `name[17]`, `latE4` i32 LE, `lonE4` i32 LE (1e-4° fixed-point; `(0,0)` = unconfigured) |
| 426 | reserved (6) |
| 432 | WireGuard block 176 B: `enabled` u8, `endpoint[65]`, `port` u16 LE, `ownPrivateKey[45]`, `peerPublicKey[45]` (base64), `ownIp[16]`, `keepalive` u16 LE |
| 608 | AC block 2 B: `beep` u8, `buttonLock` u8 |

History (read-only support; missing blocks come back as defaults):

| Ver | Size | Delta |
| --- | --- | --- |
| 1 | 112 | version u16, `ssid[33]`, `password[65]`, 12 reserved |
| 2 | 144 | reserved run replaced by the weather block |
| 3 | 432 | multi-network layout (4 slots + active index); credentials migrate to profile 0 |
| 4 | 608 | WireGuard block appended at 432 |

Strings are NUL-terminated and range-checked; any violation fails the parse
and leaves RAM settings untouched (binary payloads never merge — they are the
complete state).

## Devices payload (kind `2`)

Fixed 836 B, version `1`:

```
u16 version | u16 count | 4 records × 208 B
record: id u64 | protocolVersion u8 | reserved u8 | port u16
      | ip[4] | name[24] | tokenLen u16 | key[32] | token[≤128] | zero pad
```

`tokenLen == 0` marks an unauthenticated (V1/V2) device; records beyond
`count` are fully zeroed. The token (V3 cloud blob) and key (AES-256
handshake material) never appear in logs.

## `settings.json` — import/recovery (plaintext, transient)

Consulted **only when the encrypted store did not load** (missing, corrupt, or
unreadable — notably *not* on a foreign card, which is ignored outright). On a
successful import **and** successful encrypted write the file is **deleted**,
so credentials never linger in plaintext; a parse failure keeps it in place
for fixing and retrying at the next boot, and a failed re-encrypt import is
this-boot-only. If both `.bin` and `.bak` are valid and you want a re-import,
delete both first.
Schema version `v` (1…5 accepted); strict parse — any mistyped or oversized
field aborts the import, leaving defaults in place. A v1/v2 document
(top-level `ssid`/`pass`) migrates into profile 0.

```json
{
  "v": 5,
  "networks": [
    { "ssid": "Home", "pass": "…", "portal": 1 },
    { "ssid": "Train", "pass": "", "portal": 2 }
  ],
  "active": 0,
  "weather": { "en": 1, "name": "Berlin", "lat": 52.52, "lon": 13.405 },
  "wg": { "en": 0, "ep": "wg.example.net", "port": 51820,
          "priv": "<44-char base64>", "pub": "<44-char base64>",
          "ip": "10.0.0.5", "ka": 25 },
  "ac": { "beep": 1, "lock": 0 }
}
```

Absent keys keep defaults, *except* `networks`: a present array replaces the
whole profile list, and slot position matters (`active` indexes it).

## `device.json` — hand-editable import (plaintext, transient)

Same consult-then-delete semantics for the device store (also never consulted
while a valid `devices.bin`/`.bak` pair loads):

```json
{
  "v": 1,
  "devices": [
    { "id": 15393162840672, "name": "Living room", "version": 3,
      "ip": "192.168.1.50", "port": 6444,
      "token": "<even-length hex, <=256 chars>", "key": "<64 hex chars>" }
  ]
}
```

Rules (all-or-nothing per document): `v` required (currently `1`); ≤ 4
devices; `id` non-zero and unique (JSON number or decimal/`0x` string);
`name` printable ASCII ≤ 23 chars, default `AC-<last 4 hex of id>`;
`version` `2` or `3` when present (default 0 = autodetect); `ip` dotted quad;
`port` 1–65535 (default 6444); `token`+`key` required *together* (V3) or
absent together (V1/V2), key exactly 32 bytes hex.

## `caps.bin` — capability cache (plaintext)

Not a Vault file: it holds no secrets and any anomaly is recovered by simply
re-querying the AC, so it carries its own small header instead of A/B slots.

```
magic 'F','C','P','1' | version u8 (=1) | deviceId u64 | payloadLen u16
| crc32 u32 (over the payload) | payload = raw AcCapabilities struct
```

Single entry for the currently paired AC. `payloadLen` equals the struct size
at build time, so a firmware with a changed layout invalidates (rather than
misreads) the cache; a `deviceId` mismatch is likewise a miss.

## `provision.flag` — boot-time one-shot

Written by Settings → Hotspot (and the portal recovery flow) before rebooting
into provisioning mode. **Always deleted at the next boot**, whether or not
provisioning then succeeds — so walking away mid-provision never bootloops.

## Adding a settings field (checklist)

1. Extend `settings::Settings` + JSON ser/de in `lib/Settings` (host tests).
2. Append a binary block at the end of the payload, bump `kSchemaVersion` and
   `kBinPayloadSize`, keep readers for all legacy lengths (1:1 length↔version
   binding), update the header comment table.
3. New credential-bearing files get a new Vault `Kind`, never a new plaintext
   store.
