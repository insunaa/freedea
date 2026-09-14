# Midea protocol porting map

Reference: [mill1000/midea-msmart](https://github.com/mill1000/midea-msmart) at
commit `d7db53b` (tag `2026.8.1`), vendored for reference at `tmp/midea-msmart`
(gitignored; re-clone if missing). All `file:line` references below are for
that exact commit. This document is a **porting map**, not code: each section
lists the Python source range, the exact constants/offsets that must be
reproduced byte-for-byte, and gotchas. The C++ port lives in `lib/MideaAC/`
(host-testable, golden vectors from the Python reference).

## 0. Shared constants — `msmart/const.py`

| Constant | Line | Value |
| --- | --- | --- |
| `DISCOVERY_MSG` | 3–13 | 64 bytes, `5a5a 0111 4800 9200` + zeros + 32-byte tail `7f75bd6b…918e92e5` |
| `DEVICE_INFO_MSG` | 15–23 | 64 bytes, `5a5a 1500 0038 0004 …` (V1 only — not ported) |
| `DeviceType.AIR_CONDITIONER` | 29 | `0xAC` |
| `FrameType.CONTROL` / `QUERY` / `REPORT` / `ABNORMAL_REPORT` | 33–38 | `0x02` / `0x03` / `0x04` / `0x06` |

`DISCOVERY_MSG` is the fixed UDP broadcast payload for step 2.4. Transcribe
verbatim into a `constexpr uint8_t[]`.

## 1. CRC8 — `msmart/crc8.py:1-41` → step 2.1a

- `_CRC8_854_TABLE` (1–34): 256-entry table. **Transcribe verbatim**; do not
  assume it is derivable from a standard polynomial without checking (the name
  references poly 0x854 reversed, but the port must match the table as-is).
- `calculate(data)` (37–41): init `crc = 0`; per byte:
  `crc = table[(crc ^ byte) & 0xFF]`. Returns 0–255. No final XOR.

Port target: `constexpr uint8_t kCrc8Table[256]` (flash) + inline `crc8()`.

## 2. Frame — `msmart/frame.py:1-66` → step 2.1b

`Frame.tobytes(data)` (19–44) builds the **10-byte header + data + 1 checksum**:

| Offset | Value |
| --- | --- |
| 0 | `0xAA` start byte |
| 1 | `len(data) + 10` (header + data, **excludes** checksum byte) |
| 2 | device type (`0xAC` for AC) |
| 3–7 | `0` (unused in Python) |
| 8 | protocol version (always `0` from `Command`) |
| 9 | frame type (`0x02` control, `0x03` query) |
| 10.. | payload `data` |
| last | checksum |

`checksum(frame[1:])` (46–48): `(~sum(bytes[1 .. end-1]) + 1) & 0xFF` —
two's-complement negation of the sum **from byte 1 through the last payload
byte, exclusive of byte 0 and the checksum itself**.

`Frame.validate(frame, expected_device_type)` (50–66):
1. `len < 10` → invalid;
2. `checksum(frame[1:-1]) == frame[-1]`;
3. `frame[2] == expected_device_type`.

## 3. Command base — `msmart/device/AC/command.py:185-206`

`Command` extends `Frame` with device type `0xAC`.

- `_message_id` (190): **class-level counter**, starts 0, shared across all
  command instances. `_next_message_id()` (203–206): pre-increment, return
  `& 0xFF`. First message id sent is `0x01`.
- `tobytes(data)` (195–201): payload becomes
  `data + [message_id] + [crc8(data + [message_id])]`, then `Frame.tobytes`
  wraps it. **The CRC covers payload-with-message-id, not the frame.**

C++ implication: the message id counter must be a static counter in
`lib/MideaAC` (single writer = the comm task).

## 4. Command builders — `command.py` → step 2.3a

### GetStateCommand (226–248)

Frame type `QUERY`. 21-byte `data` (before message id/CRC):
`41 81 00 FF 03 FF 00 <temp_type> 00×12 03` where `temp_type` defaults to
`TemperatureType.INDOOR = 0x02` (179–183; `OUTDOOR = 0x03`).

### SetStateCommand (270–379)

Frame type `CONTROL`. 24-byte `data` (byte indices 0–23 below). Bit-field
construction:

- `beep = 0x40`, `power = 0x01` → byte 1 = `CONTROL_SOURCE(0x02) | beep | power`
  (`CONTROL_SOURCE = 0x2`, line 188 — "App control").
- Target temp (300–315): `modf(target)`; if `17 ≤ int ≤ 30`:
  `temperature = (int − 16) & 0xF`, `temperature_alt = 0`; else
  `temperature = 0`, `temperature_alt = (int − 12) & 0x1F`. Half-degree bit:
  `temperature |= 0x10 if frac > 0`.
- `mode = (operational_mode & 0x7) << 5` → byte 2 = `temperature | mode`.
- byte 3 = `fan_speed` raw.
- bytes 4–6 = `7F 7F 00` (timer).
- byte 7 = `0x30 | (swing_mode & 0x3F)`.
- byte 8 = `follow_me(0x80) | turbo_alt(0x20)`.
- byte 9 = `eco(0x80) | purifier(0x20) | force_aux_heat(0x10) | aux_heat(0x08)`.
- byte 10 = `sleep(0x01) | turbo(0x02) | fahrenheit(0x04)` — turbo is sent in
  **two** places (bytes 8 and 10).
- bytes 11–17 = `00 ×7`.
- byte 18 = `temperature_alt`; byte 19 = `target_humidity & 0x7F`;
  byte 20 = `00`; byte 21 = `freeze_protection ? 0x80 : 0`;
  byte 22 = `independent_aux_heat ? 0x08 : 0`; byte 23 = `00`.

Defaults (273–291): beep on, eco on, fahrenheit on, target 25.0, everything
else off/0, humidity 40.

### GetCapabilitiesCommand (208–224)

Frame type `QUERY`. `additional=false`: `data = B5 01 00`;
`additional=true`: `data = B5 01 01 01`.

### ToggleDisplayCommand (382–407)

Frame type **QUERY** despite being a control-like command (comment at 386).
`data = 41 <CONTROL_SOURCE|beep(0x40)> 00 FF 02 00 02 00 ×13`.

### Not ported

`GetGroupDataCommand` (251–268), `GetPropertiesCommand` (409–427),
`SetPropertiesCommand` (429–453), `PropertyId` encode/decode (96–177).

## 5. Response dispatch — `command.py:455-531` → step 2.3b

`Response.construct(frame)` (486–531), given a full validated frame:

1. `Frame.validate(frame, 0xAC)` (§2).
2. `frame_type = frame[9]`, `response_id = frame[10]`.
3. Dispatch: `0xC0` STATE → StateResponse; `0xB5` CAPABILITIES (only when
   `frame_type == QUERY`, devices also emit `0xB5` with frame type `0x5` —
   ignore those); `0xB0`/`0xB1` → PropertiesResponse; `0xC1` GROUP_DATA →
   Group{1,2,4,5,7,11} by `frame[13] & 0xF`.
4. Payload validation (523–526): **either** CRC8 (§1) **or** checksum (§2)
   computed over `frame[10:-2]` must equal `frame[-2]` (devices differ;
   `Response.validate` is handed `frame[10:-1]` and excludes one more byte
   itself). Properties responses are exempt (devices send bad CRCs).
5. Response class receives `frame[10:-2]` = response id (byte 0) + payload.

`ResponseId` (21–27): `B0` props-ack, `B1` props, `B5` capabilities,
`C0` state, `C1` group data.

## 6. StateResponse — `command.py:931-1065` → step 2.3b

Payload indices below are into the response payload (`frame[10:]`), so
payload[0] = response id `0xC0`.

`_parse` (977–1065):

| Byte | Fields |
| --- | --- |
| 1 | `power_on = b & 0x1` |
| 2 | `target = (b & 0xF) + 16.0`, `+= 0.5 if b & 0x10`; `mode = (b >> 5) & 0x7` |
| 3 | `fan_speed = b & 0x7F` (102 = auto, else 0–100) |
| 7 | `swing_mode = b & 0xF` |
| 8 | `turbo |= b & 0x20`; `independent_aux_heat = b & 0x40`; `follow_me = b & 0x80` |
| 9 | `eco = b & 0x10`; `purifier = b & 0x20`; `aux_heat = b & 0x08` |
| 10 | `sleep = b & 0x1`; `turbo |= b & 0x2`; `fahrenheit = b & 0x4` |
| 11 | indoor temp raw → `_parse_temperature(b, decimals=(payload[15] & 0xF)/10, fahrenheit)` |
| 12 | outdoor temp raw → `_parse_temperature(b, decimals=(payload[15] >> 4)/10, fahrenheit)` |
| 13 | `target_alt = b & 0x1F`; if ≠ 0: `target = alt + 12` (half-degree bit still from payload[2] `0x10`); `filter_alert = b & 0x20` |
| 14 | `display_on = (b != 0x70)` |
| 16 | `error_code = b` |
| 19 | `target_humidity = b & 0x7F` (only if `len(payload) ≥ 20`) |
| 21 | `freeze_protection = b & 0x80` (only if `len(payload) ≥ 22`) |

`_parse_temperature` (960–971): `0xFF` → None. Else
`temp = (data − 50) / 2`; in **Celsius** with nonzero `decimals`, replace
fraction with `±decimals`; otherwise if `decimals ≥ 0.5` use `±0.5`; else
plain `(data−50)/2`. Sign follows `temp ≥ 0`.

### CapabilitiesResponse — `command.py:532-930`

Full TLV parser `_parse_capabilities` (547–727) over `payload[1:]`:
iterates variable-length capability records keyed by `CapabilityId` (29–95,
2-byte LE id, 1-byte length, value). Port only the subset the UI needs first
(modes `0x0214`, swing `0x0215`, fan speeds `0x0210`, temperature range
`0x0225`, humidity `0x021F`, anion `0x021E`, eco/freeze-preset `0x0212`/`0x0213`,
turbo preset `0x021A`, fahrenheit `0x0222`); keep the skip-walk for unknown
IDs byte-exact. `merge()` (738–740) merges a second (additional) response.

Ported quirks (2.3b, `Responses.cpp`): decoded flags are tri-state
(`CapFlag`: absent / false / true) because Python distinguishes a never-sent
key from `False` and `merge()` only copies present keys; the `0x0225` reader
`continue`s on `size < 6` *without* advancing past the record (upstream bug,
mirrored); a record truncated by the payload end aborts the walk (Python
would raise IndexError reading the short value slice).

## 7. Security — `msmart/lan.py:650-684` → steps 2.2a/2.2b

- `SIGN_KEY` (651): ASCII `xhdiwjnchekd4d512chdjx5d8e4c394D2D7S` (36 bytes,
  **no null**). `ENC_KEY` (652): `md5(SIGN_KEY)` (16 bytes).
- `sign(data)` (676–678): `md5(data ‖ SIGN_KEY)` → 16 bytes.
- `encrypt_aes` / `decrypt_aes` (662–675): **AES-128-ECB** with `ENC_KEY`,
  PKCS#7 padding to 16 (`Padding.pad/unpad`). Padding applies on encrypt and
  is stripped on decrypt (raises on bad padding).
- `encrypt_aes_cbc` / `decrypt_aes_cbc` (654–660): **AES-CBC, IV = 16 zero
  bytes**, no padding (caller pre-pads). Key is the per-connection `local_key`.
- `udpid(device_id)` (680–684): `sha256(device_id)[:16] XOR sha256(device_id)[16:]`
  (16 bytes). Used with `device_id.to_bytes(6, "little"|"big")` — see §9.

C++: portable in-repo Md5/Sha256; AES via mbedtls family (ECB + CBC), PKCS#7
implemented and tested explicitly (pycryptodome `Padding.unpad` semantics:
pad 1..16, must repeat). Host tests use software mbedtls 3.6
(`-lmbedcrypto`) plus Python fixtures from `tmp/extract_vectors.py`.

Device AES backend (verified against the vendored framework): the arduino-esp32
C3 build sets `CONFIG_MBEDTLS_HARDWARE_AES` → `MBEDTLS_AES_ALT`, and the
prebuilt libs export **no** software `mbedtls_aes_{init,free,setkey_enc,
setkey_dec,crypt_ecb,crypt_cbc}` symbols (only `mbedtls_aes_self_test`). The
linkable AES surface is the ESP-IDF mbedtls port `esp_aes_*` from
`<aes/esp_aes.h>` (modes `ESP_AES_ENCRYPT`/`ESP_AES_DECRYPT` from
`hal/aes_types.h`); `esp_aes_context` is 34 bytes and one key schedule serves
both directions. HW AES supports 128/256-bit keys — **no 192**. The `Security`
shim selects the backend via `ESP_PLATFORM`.

## 8. V2 packet — `msmart/lan.py:686-757` (`_Packet`) → step 2.2c

`encode(device_id, command)` (690–709): AES-ECB-encrypt the whole command
frame (§7), then:

| Offset | Value |
| --- | --- |
| 0–1 | `5A 5A` start |
| 2–3 | `01 11` message type |
| 4–5 | length **LE** = `40 + len(encrypted) + 16` (incl. header, payload, hash) |
| 6–7 | `20 00` magic |
| 8–11 | message id (4 bytes, always zero) |
| 12–19 | timestamp (§below) |
| 20–27 | `device_id.to_bytes(8, "little")` — only low 6 bytes meaningful |
| 28–39 | `00 ×12` (unknown) |
| 40.. | encrypted command (AES-ECB of frame, PKCS7) |
| last 16 | `md5(packet[0:-16])` = `Security.sign` |

`decode(data)` (713–741): require `5a5a` prefix (no raw-frame fallback),
`length = LE16(data[4:6])`, trim to `length`, verify `md5(packet[:-16]) ==
packet[-16:]`, AES-ECB-decrypt `packet[40:-16]`, strip PKCS7 → frame.

`_timestamp()` (743–757): 8 bytes via `struct.pack("BBBBBBBB", ...)` in this
order: `int(microsecond/10000)`, `second`, `minute`, `hour`, `day`, `month`,
`year % 100`, `int(year/100)`. UTC. Note the **hundredths-first** ordering.
Host tests: device clock differs from fixture time — tests must not compare
the timestamp bytes, or must zero them first.

## 9. V3 packet — `msmart/lan.py:146-426` (`_LanProtocolV3`) → step 2.2d

`PacketType` (151–156): handshake req `0x0`, handshake resp `0x1`, encrypted
resp `0x3`, encrypted req `0x6`, error `0xF`.

Packet overview (verbatim comment 158–176): 6-byte header
`83 70 <size BE16> 20 (pad<<4 | type)`; payload = 2-byte request id ‖ data;
32-byte SHA256 sign over `header ‖ unencrypted payload`. The 2-byte request id
is counted in the **padding math** but not in the size field: size =
`len(data) + pad + 32`; total wire length = size + 8 (header 6 + id 2). The
receiver computes `total = BE16(buf[2:4]) + 8` (230).

`_encode_encrypted_request` (324–347): pad so `(len(data)+2) % 16 == 0`;
`length = len(data) + pad + 32`; header = `8370` + BE16(length) + `20` +
`(pad<<4 | 0x6)`; payload = BE16(packet_id) + data + **random** pad bytes;
`sha256(header ‖ payload)` computed over the **cleartext** payload; packet =
header + AES-CBC(local_key, payload) + hash.

`_encode_handshake_request` (349–359): header = `8370` + BE16(len(data)) +
`20` + `0x0`; payload = BE16(packet_id) + data (token), **unencrypted**, no
hash.

`packet_id` (375–377): increments per write, masked `& 0xFFF` (12 bits).

Decode (`data_received` 197–243, `_process_packet` 284–304,
`_decode_encrypted_response` 245–273): find `8370`, need 6-byte header,
`total = BE16(size) + 8`; type = `header[5] & 0xF`; reject if `packet[4] !=
0x20`; encrypted resp: decrypt `packet[6:-32]`, verify
`sha256(header ‖ decrypted) == packet[-32:]`, then frame =
`decrypted[2:-pad]` where `pad = header[5] >> 4`. Handshake response:
`packet[6+2:]` raw.

`_get_local_key` (379–397): handshake response payload must be 64 bytes;
`decrypted = AES-CBC(key, data[:32])`; verify `sha256(decrypted) ==
data[32:]`; **`local_key = decrypted XOR key`** (strxor, 32 bytes).

`authenticate(token, key)` (399–425): handshake request carrying the raw
token → handshake response → derive local key (above). Expiration 12 h
(149). C++: token/key are per-device config (Phase 3.4); `strxor` is a 32-byte
manual XOR loop.

Ported as `V3Packet.h/.cpp` (packet codec only; buffer framing, packet_id
counting and the `authenticate` flow land with `LanTransport` in Phase 4.2).
`encodeV3EncryptedRequest` takes the pad bytes from the caller (CSPRNG on
device, fixture on host) so it is deterministic. Upstream quirk kept-in-mind:
`decrypted[2:-pad]` with `pad == 0` yields an **empty** payload in Python;
`decodeV3EncryptedResponse` slices to the end instead — unreachable for
V2-in-V3 payloads, whose length always forces `pad = 6`. En/decrypt run
AES-CBC fully in place (mbedtls guarantees `in == out`).

## 10. Discovery — `msmart/discover.py` → step 2.4

- Broadcast: UDP `DISCOVERY_MSG` to ports **6445 and 20086** (89–99), repeated
  `_discovery_packets` times.
- `_get_device_version` (247–266): XML parses → V1 (unsupported); `5A5A` → V2;
  `8370` → V3.
- `_get_device_info` V2/V3 (269–353): for V3, first `data = data[8:-16]`
  (strip 8-byte wrapper header+id and 16 trailing bytes, 314) **before**
  the common V2 slicing; then encrypted payload = `data[40:-16]` of that
  view (V2: `len-56` bytes of the whole datagram; V3: `len-80`),
  `device_id` = little-endian int from `data[20:26]` (6 bytes),
  `AES-ECB-decrypt` → struct: IP = `data[3::-1]` (reversed 4 bytes) (334), port = `LE16(data[4:6])`,
  SN = `[8:40]` ASCII, name: `len = data[40]`, name = `[41:41+len]` ASCII,
  `device_type = int(name.split("_")[1], 16)`.
- V1 XML (`body/device/@port`) and `_authenticate_device` (355–386, cloud
  token fetch trying udpid in both LE and BE) are **not ported**; V1 raises
  `NotImplementedError` in upstream.
- **Correction to WORKPLAN 2.4**: V2/V3 discovery responses are *binary
  (AES-ECB-encrypted struct)*, not XML. XML only exists for V1 devices, which
  upstream does not support anyway. Port the binary struct parser instead.
- Local key/token for V3 devices comes from the Midea **cloud** (`getToken`) —
  out of scope; the user supplies token+key manually (WORKPLAN 3.3/3.4).
- **Ported in 2.4** (`Discovery.h/.cpp`): `kDiscoveryMessage`/ports/
  `kDiscoveryDefaultPackets` mirror `DISCOVERY_MSG` and `_send_discovery`
  (UDP send itself is Phase 3 transport). `getDiscoveryVersion` collapses the
  XML/V1 probe into `kUnsupported`. `parseDiscoveryResponse` mirrors the
  V2/V3 slicing; C++ hardens with fixed caps (160 B decrypt buffer, 39-char
  name → `kNameTooLong`, 1–4 plain-hex-digit device-type segment) and explicit
  `kTooShort`/`kDecryptFailed`/`kBadName` codes instead of Python exceptions.
  The received (source) IP is reported verbatim; upstream's embedded-IP
  mismatch warning is not ported. Goldens: `discovery.txt` (V2/V3 responses
  with byte-exact `encrypted`/`decrypted` intermediates, synthetic
  error frames).

## 11. C++ port layout (target, step 2.1+)

| C++ (lib/MideaAC/) | Python source |
| --- | --- |
| `Crc8.h` (header-only) | crc8.py |
| `Frame.h` (header-only) | frame.py |
| `Md5.h/.cpp`, `Sha256.h/.cpp` (portable, not from msmart) | RFC 1321 / FIPS 180-4 |
| `Security.h/.cpp` | lan.py 650–684 (+ PKCS7 helpers) |
| `Packet.h/.cpp` (V2) | lan.py 686–757 |
| `V3Packet.h/.cpp` (V3 codec) | lan.py 146–426 (packet layer only; session/transport in Phase 4.2) |
| `Commands.h/.cpp` | command.py 185–407 |
| `Responses.h/.cpp` | command.py 455–531, 931–1066, 532–930 (subset) |
| `AcState.h` | fixed struct mirroring StateResponse fields |
| `Discovery.h/.cpp` | discover.py V2/V3 path + const.py DISCOVERY_MSG |

Crypto portability (decided 2.2a): `md5`/`sha256` are the portable in-repo
`Md5`/`Sha256` classes compiled on **both** host and device; mbedtls is used
only for AES (device), keeping the host harness dependency-free. Golden
vectors: generate with `tmp/extract_vectors.py` + pycryptodome venv
(`tmp/msmart-venv`), one file per step under `lib/MideaAC/test/host/vectors/`.

Gotchas checklist (cross-step):
- All multi-byte lengths LE in V2/frame header size byte, but **BE16 in V3
  size field and packet_id**; device_id always LE.
- Frame checksum sums from byte 1, excluding the checksum byte.
- CRC8 covers payload+message-id, inside the frame data, not the frame.
- PKCS7 on ECB paths only; CBC paths are pre-padded by the V3 encoder.
- V3 random pad bytes: the C++ encoder takes pad bytes as a parameter, and
  `tmp/extract_vectors.py` monkey-patches `msmart.lan.get_random_bytes` to
  fixed bytes, giving a byte-exact `v3_encode_golden` (same trick as the V2
  frozen-clock golden).
