# msmart golden vectors — provenance

- **Upstream**: <https://github.com/mill1000/midea-msmart>
- **Pinned commit**: `d7db53bc470d3751b3f29a5ca2fedacdd4d14b70` (2026-09-05)
- **Source files**:
  - `msmart/tests/test_lan.py` — LAN packet vectors (V2/V3 decode + roundtrip), Security primitives
  - `msmart/device/AC/test_command.py` — frame/CRC8, state, target, capabilities, properties, group vectors
  - `msmart/tests/test_discover.py` — sample device id `15393162840672` (udpid inputs); `_DISCOVER_RESPONSES` golden V2/V3 discovery responses (`discovery.txt`)
  - `msmart/discover.py`, `msmart/const.py::DISCOVERY_MSG` — discovery logic/message itself
  - `msmart/crc8.py`, `msmart/frame.py`, `msmart/lan.py`, `msmart/device/AC/command.py` — the ported logic itself
- **Extraction**: `extract_vectors.py` (committed next to the fixtures) ran msmart *at this commit* and asserted every hex literal copied from a test file appears **verbatim** in that file's text. Computed values (CRC8, frame checksum, crypto digests) were produced by the library, not retyped.

## Fixture format

- One file per msmart test area: `frame.txt`, `crc8.txt`, `crypto.txt`, `lan.txt`, `state.txt`, `state_target.txt`, `capabilities.txt`, `dispatch.txt`, `properties.txt`, `group.txt`, `commands.txt`, `discovery.txt`.
- Column-0 line = vector name; indented `field value` lines are its fields; `  #` lines are comments; `#` at column 0 = file header (provenance).
- All hex is lowercase. Temperatures are the exact Python `repr()` of the parsed float. `none` means the field is absent/not decoded; capability flags use `absent` for keys the device never sent (Python dict keys), distinct from `0`.
- `capabilities.txt` merge-pair vectors (`*_merged`) carry `main_frame` + `followup_frame` and assert the merged result; `reader_*` vectors are synthetic single-record payloads (field `payload`, no frame) exercising one capability reader each.
- `discovery.txt`: `discovery_msg` pins the broadcast payload/ports/count; `v2_response`/`v3_response` are the `test_discover.py` datagrams and assert version, `encrypted`/`decrypted` intermediates, and all parsed fields (`device_id`/`port` decimal, `device_type` hex, `name`, `sn`, `source_ip`). `xml_response`/`prefix_unknown`/`single_byte` assert `getDiscoveryVersion` = 0 (`version` field; `0` covers both XML and unknown). `error=` vectors (decrypt|name|name_too_long) are synthetic V2 rebuilds (re-encrypted structs or a flipped byte) pinning C++-hardened error paths where upstream just raises; upstream was executed on each and observed to raise (or, for `v2_long_name`, to accept the long name).
- `dispatch.txt` vectors assert `classifyResponse` outcomes (`expect_kind` state|capabilities|properties|properties_ack|group_data|none|invalid, plus `expect_group` for group data); frames there may be checksum-refix edited variants (e.g. unsolicited 0xB5, bad payload tail).
- **Do not edit by hand.** Re-generate and review the diff instead.

## Determinism notes (read before writing C++ tests)

- **Decode paths are deterministic** and are the golden targets: `v2_decode`, `v3_decode`, state/capabilities/properties/group frames, CRC8, and raw crypto primitives (`sign`, AES-ECB, `udpid`).
- **Encode paths are NOT golden packets**:
  - V2 `_Packet.encode` embeds a live UTC timestamp (header bytes 12..20).
  - V3 `_LanProtocolV3._encode_encrypted_request` uses random pad bytes.
  - `Command.tobytes` consumes the class-level `_message_id` counter on every
    call: `commands.txt` vectors pin `Command._message_id` before each build
    and record it as `message_id_counter`; C++ tests call
    `midea::resetMessageId()` with that value before `serialize()`.
  - C++ encode tests must therefore verify header fields, ciphertext, and hash tail *separately* (fixture frames give the expected ciphertext/tail for a fixed plaintext).
- V3 state payload tails validate as **CRC8 *or* frame checksum** (`command.py:479`) — see `crc8.txt:state_v3_payload_tail` (`matched_by crc8`) and `state.txt:state_v3_crc_tail`.

## Regeneration

From the repo root (`tmp/` is the gitignored scratch area, see
`docs/devcontainer-setup.md` §6):

```sh
git clone https://github.com/mill1000/midea-msmart tmp/midea-msmart
git -C tmp/midea-msmart checkout d7db53bc470d3751b3f29a5ca2fedacdd4d14b70
python3 -m venv tmp/msmart-venv && tmp/msmart-venv/bin/pip install pycryptodome httpx
tmp/msmart-venv/bin/python lib/MideaAC/test/host/vectors/extract_vectors.py d7db53bc470d3751b3f29a5ca2fedacdd4d14b70
git diff --exit-code lib/MideaAC/test/host/vectors/   # must be clean if upstream is unchanged
```
