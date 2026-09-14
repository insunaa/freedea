#!/usr/bin/env python3
"""One-shot extractor: midea-msmart golden vectors -> freedea host fixtures.

Run from the repo root against a clone of msmart at the pinned commit
(see vectors/NOTES.md for the full recipe):

    tmp/msmart-venv/bin/python lib/MideaAC/test/host/vectors/extract_vectors.py \
        <full-sha> [msmart-dir]        # msmart-dir defaults to tmp/midea-msmart

Every vector copied from a Python test file is asserted to appear VERBATIM in
that file's text (byte-identity by construction). Computed vectors (frames,
crypto digests, CRC8) are produced by running msmart at the recorded commit
and cross-checked against the test expectations where one exists.
"""
import hashlib
import pathlib
import sys

MSMART = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "tmp/midea-msmart")
# Fixtures live next to this script (vectors/), so regeneration works from a
# fresh clone with no absolute paths baked in.
OUT = pathlib.Path(__file__).resolve().parent
SHA = sys.argv[1]

sys.path.insert(0, str(MSMART))
from msmart.crc8 import calculate as crc8_calc
from msmart.frame import Frame, InvalidFrameException
from msmart.const import DeviceType
import msmart.lan as lan_mod
from msmart.lan import Security, _Packet, _LanProtocolV3
from msmart.device.AC.command import (
    CapabilityId,
    Command,
    GetCapabilitiesCommand,
    GetGroupDataCommand,
    GetStateCommand,
    SetStateCommand,
    ToggleDisplayCommand,
    Response,
    StateResponse,
    CapabilitiesResponse,
    PropertiesResponse,
    Group4Response,
    Group5Response,
    Group1Response,
    Group2Response,
    Group7Response,
    Group11Response,
    InvalidResponseException,
)

SRC_CMD = MSMART / "msmart" / "device" / "AC" / "test_command.py"
SRC_LAN = MSMART / "msmart" / "tests" / "test_lan.py"
SRC_DISC = MSMART / "msmart" / "tests" / "test_discover.py"
SRC_CMD_TEXT = SRC_CMD.read_text()
SRC_LAN_TEXT = SRC_LAN.read_text()


def in_src(hexstr, name):
    assert hexstr in name, "NOT in source: " + hexstr[:32]


def write(rel, lines):
    p = OUT / rel
    p.write_text("\n".join(lines) + "\n")
    print("wrote", p.relative_to(OUT.parent.parent.parent.parent), len(lines), "lines")


def hdr(title, source):
    return [
        "# " + title,
        "# Source: midea-msmart @ " + SHA + " " + source,
        "# Format: column-0 line = vector name; indented `field value` lines follow.",
        "# Hex values are lowercase. Do not edit by hand; re-run the extractor.",
        "",
    ]


def vec(name, fields, comment=None):
    out = [name]
    if comment:
        out.append("  # " + comment)
    for k, v in fields:
        out.append("  " + k + " " + v)
    out.append("")
    return out


# ---------------------------------------------------------------- frame.txt
print("== frame vectors")
GETSTATE_PAYLOAD = "418100ff03ff00020000000000000000000000000311f4"
in_src(GETSTATE_PAYLOAD, SRC_CMD_TEXT)
Command._message_id = 0x10
getstate_frame = GetStateCommand().tobytes()
assert getstate_frame[10:-1].hex() == GETSTATE_PAYLOAD, "frame/payload mismatch"
assert getstate_frame[0] == 0xAA and getstate_frame[2] == 0xAC
assert getstate_frame[1] == len(getstate_frame) - 1, "length byte"
Frame.validate(memoryview(getstate_frame), DeviceType.AIR_CONDITIONER)

V2_ROUNDTRIP_FRAME = "aa21ac8d000000000003418100ff03ff000200000000000000000000000003016971"
in_src(V2_ROUNDTRIP_FRAME, SRC_LAN_TEXT)
Frame.validate(memoryview(bytes.fromhex(V2_ROUNDTRIP_FRAME)), DeviceType.AIR_CONDITIONER)

write("frame.txt", hdr("GetState command frame vectors", "msmart/device/AC/test_command.py::test_frame, msmart/tests/test_lan.py") + vec(
    "getstate_full",
    [("frame", getstate_frame.hex()), ("payload", GETSTATE_PAYLOAD), ("message_id_counter", "10"), ("message_id_byte", "11"), ("payload_crc8", "f4")],
    "Command._message_id=0x10, post-incremented to 0x11 in payload; frame=header(10)+payload+csum") + vec(
    "getstate_payload",
    [("payload", GETSTATE_PAYLOAD)],
    "test_frame EXPECTED_PAYLOAD, byte-identical to Python source") + vec(
    "v2_roundtrip_frame",
    [("frame", V2_ROUNDTRIP_FRAME)],
    "test_encode_packet_roundtrip FRAME (protocol byte 0x8d), byte-identical to Python source"))

# ---------------------------------------------------------------- crc8.txt
print("== crc8 vectors")
lines = hdr("CRC8 (854 table) vectors", "msmart/crc8.py, msmart/device/AC/command.py:201,479")
gp = bytes.fromhex(GETSTATE_PAYLOAD)
assert crc8_calc(gp[:-1]) == gp[-1] == 0xF4
lines += vec("getstate_payload", [("input", gp[:-1].hex()), ("expected", "%02x" % gp[-1])], "crc8 over payload minus final crc byte (command.py:201)")

state_v3 = bytes.fromhex("aa23ac00000000000303c00145660000003c0010045c6b20000000000000000000020d79")
in_src(state_v3.hex(), SRC_CMD_TEXT)
sp = state_v3[10:-1]
crc = crc8_calc(sp[:-1])
chk = Frame.checksum(sp[:-1])
print("state_v3 payload tail=%02x crc8=%02x checksum=%02x" % (sp[-1], crc, chk))
which = "crc8" if sp[-1] == crc else ("checksum" if sp[-1] == chk else "MISMATCH")
lines += vec("state_v3_payload_tail", [("input", sp[:-1].hex()), ("expected", "%02x" % sp[-1]), ("matched_by", which)], "Response.validate: tail must equal crc8 OR frame checksum (command.py:479)")

z16 = bytes(16)
lines += vec("zero16", [("input", z16.hex()), ("expected", "%02x" % crc8_calc(z16))])
lines += vec("byte_01", [("input", "01"), ("expected", "%02x" % crc8_calc(b"\x01"))])
write("crc8.txt", lines)

# ---------------------------------------------------------------- crypto.txt
print("== crypto vectors")
SIGN_KEY = Security.SIGN_KEY
ENC_KEY = Security.ENC_KEY
assert SIGN_KEY == b"xhdiwjnchekd4d512chdjx5d8e4c394D2D7S"
assert ENC_KEY == hashlib.md5(SIGN_KEY).digest()

frame33 = bytes.fromhex(V2_ROUNDTRIP_FRAME)
enc33 = Security.encrypt_aes(frame33)
assert Security.decrypt_aes(enc33) == frame33
e16 = Security.encrypt_aes(z16)
assert Security.decrypt_aes(e16) == z16
e32 = Security.encrypt_aes(bytes(32))
assert Security.decrypt_aes(e32) == bytes(32)

DEV_ID = 15393162840672  # msmart/tests/test_discover.py
assert str(DEV_ID) in SRC_DISC.read_text()
le = DEV_ID.to_bytes(6, "little")
be = DEV_ID.to_bytes(6, "big")
up_le = Security.udpid(le)
up_be = Security.udpid(be)
sh = hashlib.sha256(le).digest()
assert up_le == bytes(a ^ b for a, b in zip(sh[:16], sh[16:]))

lines = hdr("Security primitive vectors (V2)", "msmart/lan.py Security: SIGN_KEY/ENC_KEY/sign/encrypt_aes/encrypt_aes_cbc/udpid")
lines += vec("sign_key", [("value", SIGN_KEY.hex()), ("ascii", SIGN_KEY.decode())], "lan.py:651")
lines += vec("enc_key", [("value", ENC_KEY.hex())], "md5(SIGN_KEY), lan.py:652")
lines += vec("sign_frame33", [("input", frame33.hex()), ("expected", Security.sign(frame33).hex())], "sign(d)=md5(d+SIGN_KEY), lan.py:677")
lines += vec("sign_empty", [("input", ""), ("expected", Security.sign(b"").hex())], "sign of empty data")
lines += vec("aes_ecb_zero16", [("input", z16.hex()), ("expected", e16.hex())], "AES-128-ECB key=ENC_KEY, PKCS7, lan.py:660-668")
lines += vec("aes_ecb_zero32", [("input", bytes(32).hex()), ("expected", e32.hex())], "2-block")
lines += vec("aes_ecb_frame33", [("input", frame33.hex()), ("expected", enc33.hex())], "33B->48B; ciphertext sits at packet[40:88] of any V2 packet carrying this frame")

# AES-CBC zero-IV vectors (V3 local_key path), lan.py:654-660
k32 = bytes(range(32))
def _cbcv(name, key, data, note):
    enc = Security.encrypt_aes_cbc(key, data)
    assert Security.decrypt_aes_cbc(key, enc) == data
    return vec(name, [("key", key.hex()), ("input", data.hex()), ("expected", enc.hex())], note)
lines += _cbcv("aes_cbc128_pattern16", ENC_KEY, bytes(range(16)), "AES-128-CBC zero-IV, lan.py:654-660")
lines += _cbcv("aes_cbc128_zeros32", ENC_KEY, bytes(32), "2-block")
lines += _cbcv("aes_cbc256_pattern48", k32, bytes(range(48)), "AES-256-CBC zero-IV (V3 local_key is 32B)")
lines += vec("udpid_device_le", [("device_id", "%012x" % DEV_ID), ("input", le.hex()), ("expected", up_le.hex())], "sha256(id)[:16]^sha256(id)[16:], 6-byte LE id")
lines += vec("udpid_device_be", [("device_id", "%012x" % DEV_ID), ("input", be.hex()), ("expected", up_be.hex())], "same, 6-byte BE id (discover.py tries both)")
write("crypto.txt", lines)

# ---------------------------------------------------------------- lan.txt
print("== lan packet vectors")
V2_PACKET = "5a5a01116800208000000000000000000000000060ca0000000e0000000000000000000001000000c6a90377a364cb55af337259514c6f96bf084e8c7a899b50b68920cdea36cecf11c882a88861d1f46cd87912f201218c66151f0c9fbe5941c5384e707c36ff76"
V2_PACKET_FRAME = "aa22ac00000000000303c0014566000000300010045cff2070000000000000008bed19"
in_src(V2_PACKET, SRC_LAN_TEXT)
in_src(V2_PACKET_FRAME, SRC_LAN_TEXT)
assert _Packet.decode(bytes.fromhex(V2_PACKET)) == bytes.fromhex(V2_PACKET_FRAME)

# Byte-exact encode golden: _Packet.encode embeds a live UTC timestamp, so
# monkey-patch _timestamp with fixed distinct bytes (lan.py order: hundredths,
# second, minute, hour, day, month, year%100, year//100).
V2_TS = bytes([42, 59, 58, 23, 29, 6, 25, 20])
_orig_timestamp = _Packet._timestamp
_Packet._timestamp = classmethod(lambda cls: V2_TS)
try:
    V2_ENCODE_GOLDEN = _Packet.encode(123456, bytes.fromhex(V2_ROUNDTRIP_FRAME)).hex()
finally:
    _Packet._timestamp = _orig_timestamp
assert _Packet.decode(bytes.fromhex(V2_ENCODE_GOLDEN)) == bytes.fromhex(V2_ROUNDTRIP_FRAME)

V3_PACKET = "8370008e2063ec2b8aeb17d4e3aff77094dde7fa65cf22671adf807f490a97b927347943626e9b4f58362cf34b97a0d641f8bf0c8fcbf69ad8cca131d2d7baa70ef048c5e3f3dc78da8af4598ff47aee762a0345c18815d91b50a24dedcacde0663c4ec5e73a963dc8bbbea9a593859996eb79dcfcc6a29b96262fcaa8ea6346366efea214e4a2e48caf83489475246b6fef90192b00"
V3_LOCAL_KEY = "55a0a178746a424bf1fc6bb74b9fb9e4515965048d24ce8dc72aca91597d05ab"
V3_PAYLOAD = "5a5a01116800208000000000eaa908020c0817143daa0000008600000000000000000180000000003e99f93bb0cf9ffa100cb24dbae7838641d6e63ccbcd366130cd74a372932526d98479ff1725dce7df687d32e1776bf68a3fa6fd6259d7eb25f32769fcffef78"
V3_FRAME = "aa23ac00000000000303c00145660000003c0010045c6800000000000000000000018426"
for h in (V3_PACKET, V3_LOCAL_KEY, V3_PAYLOAD, V3_FRAME):
    in_src(h, SRC_LAN_TEXT)
proto = _LanProtocolV3()
proto._local_key = bytes.fromhex(V3_LOCAL_KEY)
payload = proto._process_packet(memoryview(bytes.fromhex(V3_PACKET)))
assert payload == bytes.fromhex(V3_PAYLOAD)
assert _Packet.decode(payload) == bytes.fromhex(V3_FRAME)

# Byte-exact V3 encrypted-request golden: _encode_encrypted_request pads with
# get_random_bytes; monkey-patch it to fixed pad bytes (the C++ encoder takes
# pad bytes from the caller, so it is deterministic given the same pad).
V3_ENC_DATA = bytes.fromhex(V2_ENCODE_GOLDEN)  # 104B -> (104+2)%16=10 -> pad 6
V3_ENC_ID = 5555
V3_FIXED_PAD = bytes([0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6])
_orig_rand = lan_mod.get_random_bytes
lan_mod.get_random_bytes = lambda n: V3_FIXED_PAD[:n]
try:
    V3_ENC_GOLDEN = proto._encode_encrypted_request(V3_ENC_ID, V3_ENC_DATA).hex()
finally:
    lan_mod.get_random_bytes = _orig_rand
assert proto._decode_encrypted_response(memoryview(bytes.fromhex(V3_ENC_GOLDEN))) == V3_ENC_DATA

# Handshake request golden: fully deterministic (no pad, no hash).
V3_HS_TOKEN = bytes.fromhex("00112233445566778899aabbccddeeff0123456789abcdef0123456789abcdef")
V3_HS_ID = 1
V3_HS_GOLDEN = proto._encode_handshake_request(V3_HS_ID, V3_HS_TOKEN).hex()

# Local-key derivation golden (synthesize a 64-byte handshake response).
LK_KEY = bytes(range(32))
LK_PLAIN = bytes(range(32, 64))
LK_DATA = Security.encrypt_aes_cbc(LK_KEY, LK_PLAIN) + hashlib.sha256(LK_PLAIN).digest()
assert len(LK_DATA) == 64
LK_LOCAL = proto._get_local_key(LK_KEY, memoryview(LK_DATA))
assert LK_LOCAL == bytes(a ^ b for a, b in zip(LK_PLAIN, LK_KEY))

lines = hdr("LAN packet vectors (V2/V3)", "msmart/tests/test_lan.py TestEncodeDecode")
lines += vec("v2_decode", [("packet", V2_PACKET), ("expected_frame", V2_PACKET_FRAME)], "test_decode_packet; decode verifies md5 tail then AES-ECB decrypt")
lines += vec("v2_roundtrip", [("frame", V2_ROUNDTRIP_FRAME), ("device_id", "123456")], "test_encode_packet_roundtrip; encode() embeds live UTC timestamp -> packet NOT a golden; C++ test must verify header fields + ciphertext + md5 tail")
lines += vec("v2_encode_golden", [("frame", V2_ROUNDTRIP_FRAME), ("device_id", "123456"), ("timestamp", V2_TS.hex()), ("expected_packet", V2_ENCODE_GOLDEN)], "encode() with _timestamp() monkey-patched (frozen clock) -> byte-exact golden packet")
lines += vec("v3_decode", [("packet", V3_PACKET), ("local_key", V3_LOCAL_KEY), ("expected_payload", V3_PAYLOAD), ("expected_frame", V3_FRAME)], "test_decode_v3_packet; payload is a full V2 packet (sha256 tail over payload)")
lines += vec("v3_roundtrip", [("frame", V3_FRAME), ("local_key", V3_LOCAL_KEY), ("packet_id", "5555"), ("device_id", "123456")], "test_encode_packet_v3_roundtrip; encode uses random pad bytes -> packet NOT a golden; deterministic part: sha256(header+payload) tail + AES-CBC(zero IV)")
lines += vec("v3_encode_golden", [("data", V3_ENC_DATA.hex()), ("local_key", V3_LOCAL_KEY), ("packet_id", str(V3_ENC_ID)), ("pad", V3_FIXED_PAD.hex()), ("expected_packet", V3_ENC_GOLDEN)], "_encode_encrypted_request with get_random_bytes monkey-patched to fixed pad -> byte-exact golden; data is the v2_encode_golden packet")
lines += vec("v3_handshake_golden", [("token", V3_HS_TOKEN.hex()), ("packet_id", str(V3_HS_ID)), ("expected_packet", V3_HS_GOLDEN)], "_encode_handshake_request: unencrypted, size field = token length, no hash")
lines += vec("v3_local_key", [("key", LK_KEY.hex()), ("handshake_data", LK_DATA.hex()), ("expected_local_key", LK_LOCAL.hex())], "_get_local_key: local_key = sha256-verified AES-CBC-decrypted data[:32] XOR key (64-byte handshake response)")
write("lan.txt", lines)

# ---------------------------------------------------------------- state.txt
print("== state vectors")
def fmt_bool(v):
    return "none" if v is None else ("1" if v else "0")


def fmt_int(v):
    return "none" if v is None else str(v)


def fmt_float(v):
    return "none" if v is None else repr(v)


def state_expect_fields(resp):
    # Mirrors every AcState field the C++ StateResponse parser produces.
    return [
        ("expect_power_on", fmt_bool(resp.power_on)),
        ("expect_target", fmt_float(resp.target_temperature)),
        ("expect_operational_mode", fmt_int(resp.operational_mode)),
        ("expect_fan_speed", fmt_int(resp.fan_speed)),
        ("expect_swing_mode", fmt_int(resp.swing_mode)),
        ("expect_turbo", fmt_bool(resp.turbo)),
        ("expect_eco", fmt_bool(resp.eco)),
        ("expect_sleep", fmt_bool(resp.sleep)),
        ("expect_fahrenheit", fmt_bool(resp.fahrenheit)),
        ("expect_indoor", fmt_float(resp.indoor_temperature)),
        ("expect_outdoor", fmt_float(resp.outdoor_temperature)),
        ("expect_filter_alert", fmt_bool(resp.filter_alert)),
        ("expect_display_on", fmt_bool(resp.display_on)),
        ("expect_error_code", fmt_int(resp.error_code)),
        ("expect_follow_me", fmt_bool(resp.follow_me)),
        ("expect_purifier", fmt_bool(resp.purifier)),
        ("expect_aux_heat", fmt_bool(resp.aux_heat)),
        ("expect_independent_aux_heat", fmt_bool(resp.independent_aux_heat)),
        ("expect_target_humidity", fmt_int(resp.target_humidity)),
        ("expect_freeze_protection", fmt_bool(resp.freeze_protection)),
    ]


lines = hdr("State response vectors", "msmart/device/AC/test_command.py TestStateResponse")


def state_vec(name, hexstr, target, indoor, outdoor, comment):
    in_src(hexstr, SRC_CMD_TEXT)
    resp = Response.construct(bytes.fromhex(hexstr))
    assert type(resp) is StateResponse, name
    assert resp.target_temperature == target, (name, target)
    assert resp.indoor_temperature == indoor, (name, indoor)
    assert resp.outdoor_temperature == outdoor, (name, outdoor)
    return vec(name, [("frame", hexstr)] + state_expect_fields(resp), comment)


lines += state_vec("state_v2", "aa22ac00000000000303c0014566000000300010045eff00000000000000000069fdb9", 21.0, 22.0, None, "test_message_v2")
lines += state_vec("state_v3", "aa23ac00000000000303c00145660000003c0010045c6b20000000000000000000020d79", 21.0, 21.0, 28.5, "test_message_v3; payload tail validated as crc8 OR checksum")
lines += state_vec("state_v3_crc_tail", "aa1eac00000000000003c0004b1e7f7f000000000069630000000000000d33", 27.0, 27.5, 24.5, "test_message_checksum; shorter-than-expected V3 state, tail is crc8")
for tag, hexstr, t, i, o in [
    ("precision_1", "aa23ac00000000000203c00188647f7f000000000063450c0056190000000000000497c3", 24.0, 24.6, 9.5),
    ("precision_2", "aa23ac00000000000203c00188647f7f000000000067450c00750000000000000001a3b0", 24.0, 26.5, 9.7),
    ("precision_3", "aa23ac00000000000203c00188647f7f000080000064450c00501d00000000000001508e", 24.0, 25.0, 9.5),
]:
    lines += state_vec(tag, hexstr, t, i, o, "test_message_additional_precision; extra precision bits")
write("state.txt", lines)

# target-only raw payloads
TARGETS = [
    ("c00181667f7f003c00000060560400420000000000000048", 16.0),
    ("c00191667f7f003c00000060560400440000000000000049", 16.5),
    ("c00181667f7f003c0000006156050036000000000000004a", 17.0),
    ("c00191667f7f003c0000006156050028000000000000004b", 17.5),
    ("c00182667f7f003c0000006156060028000000000000004c", 18.0),
    ("c00192667f7f003c0000006156060028000000000000004d", 18.5),
    ("c00183667f7f003c0000006156070028000000000000004e", 19.0),
    ("c00193667f7f003c00000061570700550000000000000050", 19.5),
    ("c00040660000003c00000062680400000000000000000004", 16.0),
    ("c00050660000003c00000062670400000000000000000004", 16.5),
]
for hexstr, t in TARGETS:
    in_src(hexstr, SRC_CMD_TEXT)
    assert StateResponse(memoryview(bytes.fromhex(hexstr))).target_temperature == t
lines = hdr("State target-temperature raw payloads (no frame)", "msmart/device/AC/test_command.py::test_target_temperature")
for idx, (hexstr, t) in enumerate(TARGETS):
    resp = StateResponse(memoryview(bytes.fromhex(hexstr)))
    lines += vec("target_%d" % idx, [("payload", hexstr)] + state_expect_fields(resp), "U-shaped unit" if idx >= 8 else "raw StateResponse payload")
write("state_target.txt", lines)

# ---------------------------------------------------------------- capabilities.txt
print("== capabilities vectors")
CAPS = [
    ("caps_1", "aa29ac00000000000303b5071202010113020101140201011502010116020101170201001a020101dedb", "test_capabilities"),
    ("caps_2", "aa3dac00000000000203b50a12020101180001001402010115020101160201001a020101100201011f020100250207203c203c203c00400001000100c83a", "test_capabilities_2"),
    ("caps_3", "aa29ac00000000000303b507120201021402010015020102170201021a0201021002010524020101990d", "test_capabilities_3 (Toshiba window unit)"),
    ("caps_4", "aa39ac00000000000303b50912020102130201001402010015020100170201021a02010010020101250207203c203c203c00240201010102a1a0", "test_capabilities_4 (U-shaped window unit)"),
    ("caps_additional_main", "aa3dac00000000000303b50a12020101430001011402010115020101160201001a020101100201011f020103250207203c203c203c05400001000100c805", "test_additional_capabilities; more=1"),
    ("caps_additional_followup", "aa23ac00000000000303b5051e020101130201012202010019020100390001010000febe", "test_additional_capabilities; merge target"),
    ("caps_aux_heat_main", "aa29ac00000000000303b50514020109150201021a020101250207203c203c203c003402010101007b1d", "test_capabilities_aux_heat"),
    ("caps_aux_heat_followup", "aa2fac00000000000303b508100201051f020100300001001302010019020101390001009300010194000101000095ca", "test_capabilities_aux_heat"),
    ("caps_flash_1", "aa27ac00000000000303b5051f0201002c020101670001011602010451000101e30001010004f564", "test_capabilities_flash"),
    ("caps_flash_2", "aa56ac00000000000803b51012020100180001001402010115020101160201041a020101100201011f020103250207203c203c203c0551000101e30002080867000102c200010098000101950001019d00010101008b43", "test_capabilities_flash; ieco 8 levels"),
    ("caps_flash_3", "aa52ac00000000000803b50f120201001402010015020101160201041a02010110020101250207203c203c203c002402010151000101e3000208086700010495000101980001017c000100cd0001000100bd47", "test_capabilities_flash / test_capabilities_ieco_ecomaster (identical)"),
    ("caps_cascade", "aa3bac00000000000303b50a1e02010113020101220201001902010039000101580001024200010159000101090001010a000101000000000000cfbf", "test_capabilities_cascade"),
    ("caps_out_silent", "aa2fac00000000000803b5081f0201002c020101160201043900010151000101e300010113020101cd0001030002365b", "test_capabilities_out_silent (PortaSplit)"),
    ("caps_ieco", "aa2bac00000000000803b5071f0201002c020101160201043900010151000101e3000101130201010002fa6d", "test_capabilities_ieco; ieco_number=1"),
]
# Capability flags parsed by the C++ port (subset per docs/porting-notes.md sec. 6).
CAP_SUBSET = [
    "anion", "eco", "freeze_protection", "fahrenheit",
    "heat_mode", "cool_mode", "dry_mode", "auto_mode", "aux_heat_mode", "aux_mode",
    "swing_horizontal", "swing_vertical",
    "fan_silent", "fan_low", "fan_medium", "fan_high", "fan_auto", "fan_custom",
    "humidity_auto_set", "humidity_manual_set",
    "turbo_heat", "turbo_cool",
    "cool_min_temperature", "cool_max_temperature", "auto_min_temperature",
    "auto_max_temperature", "heat_min_temperature", "heat_max_temperature", "decimals",
]


def caps_resp(hexstr):
    r = Response.construct(bytes.fromhex(hexstr))
    assert type(r) is CapabilitiesResponse, hexstr[:16]
    return r


def caps_expect_fields(resp):
    out = []
    c = resp._capabilities
    for k in CAP_SUBSET:
        if k not in c:
            out.append(("expect_" + k, "absent"))
        elif isinstance(c[k], float):
            out.append(("expect_" + k, repr(c[k])))
        else:
            out.append(("expect_" + k, "1" if c[k] else "0"))
    out.append(("expect_additional", "1" if resp.additional_capabilities else "0"))
    return out


lines = hdr("Capabilities response vectors", "msmart/device/AC/test_command.py TestCapabilitiesResponse")
CAPS_BY_TAG = {}
for tag, hexstr, note in CAPS:
    in_src(hexstr, SRC_CMD_TEXT)
    CAPS_BY_TAG[tag] = hexstr
    lines += vec(tag, [("frame", hexstr)] + caps_expect_fields(caps_resp(hexstr)), note)

# Merge pairs: dict.update semantics (followup overwrites only keys it carries).
for prefix, main_tag, foll_tag, note in [
    ("caps_additional", "caps_additional_main", "caps_additional_followup", "test_additional_capabilities; main merged with followup"),
    ("caps_aux_heat", "caps_aux_heat_main", "caps_aux_heat_followup", "test_capabilities_aux_heat; main merged with followup"),
]:
    merged = caps_resp(CAPS_BY_TAG[main_tag])
    merged.merge(caps_resp(CAPS_BY_TAG[foll_tag]))
    lines += vec(prefix + "_merged", [("main_frame", CAPS_BY_TAG[main_tag]), ("followup_frame", CAPS_BY_TAG[foll_tag])] + caps_expect_fields(merged), note)

# Synthetic single-capability payloads (test_capabilities_parsers readers).
for name, cap, value, expect in [
    ("reader_eco_0", CapabilityId.PRESET_ECO, 0, {"eco": False}),
    ("reader_eco_1", CapabilityId.PRESET_ECO, 1, {"eco": True}),
    ("reader_eco_2", CapabilityId.PRESET_ECO, 2, {"eco": True}),
    ("reader_turbo_0", CapabilityId.PRESET_TURBO, 0, {"turbo_heat": False, "turbo_cool": True}),
    ("reader_turbo_1", CapabilityId.PRESET_TURBO, 1, {"turbo_heat": True, "turbo_cool": True}),
    ("reader_turbo_3", CapabilityId.PRESET_TURBO, 3, {"turbo_heat": True, "turbo_cool": False}),
    ("reader_turbo_4", CapabilityId.PRESET_TURBO, 4, {"turbo_heat": False, "turbo_cool": False}),
]:
    data = b"\xBA\x01" + cap.to_bytes(2, "little") + b"\x01" + bytes([value])
    r = CapabilitiesResponse(memoryview(data))
    for k, v in expect.items():
        assert r._capabilities[k] == v, (name, k)
    lines += vec(name, [("payload", data.hex())] + [("expect_" + k, "1" if v else "0") for k, v in expect.items()], "test_capabilities_parsers synthetic reader payload")
write("capabilities.txt", lines)

# ---------------------------------------------------------------- dispatch.txt
print("== dispatch vectors")
lines = hdr("Response dispatch (Response.construct) vectors", "msmart/device/AC/command.py:486-531")

STATE_FRAME = bytes.fromhex("aa22ac00000000000303c0014566000000300010045eff00000000000000000069fdb9")
CAPS_FRAME = bytes.fromhex("aa29ac00000000000303b5071202010113020101140201011502010116020101170201001a020101dedb")
PROPS_FRAME = bytes.fromhex("aa21ac00000000000303b10409000001000a00000100150000012b1e020000005fa3")
PROPS_ACK_FRAME = bytes.fromhex("aa18ac00000000000302b0020a0000013209001101000089a4")
GROUP_FRAME = bytes.fromhex("aa1fac00000000000303c121014400067920000000000000000000000000aabf")


def refix(frame):
    """Rewrite the frame checksum after in-place payload edits."""
    f = bytearray(frame)
    f[-1] = (~sum(f[1:-1]) + 1) & 0xFF
    return bytes(f)


def payload_tail_fails(frame):
    sp = frame[10:-1]
    return crc8_calc(sp[:-1]) != sp[-1] and Frame.checksum(sp[:-1]) != sp[-1]


def dispatch_vec(name, frame, want, comment):
    try:
        r = Response.construct(bytes(frame))
        rid = frame[10]
        cls = type(r)
        if cls is StateResponse:
            kind, group = "state", None
        elif cls is CapabilitiesResponse:
            kind, group = "capabilities", None
        elif cls is PropertiesResponse:
            kind, group = ("properties_ack" if rid == 0xB0 else "properties"), None
        elif cls.__name__.startswith("Group"):
            kind, group = "group_data", frame[13] & 0xF
        else:
            kind, group = "none", None
    except InvalidResponseException:
        kind, group = "invalid", None
    assert kind == want, (name, kind)
    fields = [("frame", frame.hex()), ("expect_kind", kind)]
    if group is not None:
        fields.append(("expect_group", str(group)))
    return vec(name, fields, comment)


lines += dispatch_vec("dispatch_state", STATE_FRAME, "state", "StateResponse dispatch on id 0xC0")
lines += dispatch_vec("dispatch_caps", CAPS_FRAME, "capabilities", "CapabilitiesResponse on 0xB5 + QUERY frame type")
f = bytearray(CAPS_FRAME)
f[9] = 0x05
lines += dispatch_vec("dispatch_caps_unsolicited", refix(f), "none", "0xB5 with frame type 0x5 is ignored (command.py:515)")
f = bytearray(STATE_FRAME)
f[10] = 0xEE
f[-2] = crc8_calc(f[10:-2])
lines += dispatch_vec("dispatch_unknown_id", refix(f), "none", "unrecognized response id -> base Response")
lines += dispatch_vec("dispatch_props", PROPS_FRAME, "properties", "PropertiesResponse on 0xB1")
lines += dispatch_vec("dispatch_props_ack", PROPS_ACK_FRAME, "properties_ack", "PropertiesResponse (ack) on 0xB0")
f = bytearray(PROPS_FRAME)
f[11] ^= 0xFF
f = refix(f)
assert payload_tail_fails(f), "props tail must fail CRC+checksum"
lines += dispatch_vec("dispatch_props_bad_tail", f, "properties", "Properties are exempt from payload CRC validation")
f = bytearray(STATE_FRAME)
f[11] ^= 0xFF
f = refix(f)
assert payload_tail_fails(f)
lines += dispatch_vec("dispatch_state_bad_tail", f, "invalid", "state frame failing both CRC8 and checksum")
lines += dispatch_vec("dispatch_group", GROUP_FRAME, "group_data", "Group4; classify also reports frame[13] & 0xF")
write("dispatch.txt", lines)

# ---------------------------------------------------------------- properties.txt
print("== properties vectors")
lines = hdr("Properties response vectors", "msmart/device/AC/test_command.py TestPropertiesResponse/TestResponseConstruct")
r = Response.construct(bytes.fromhex("aa21ac00000000000303b10409000001000a00000100150000012b1e020000005fa3"))
assert type(r) is PropertiesResponse
lines += vec("props_parsing", [("frame", "aa21ac00000000000303b10409000001000a00000100150000012b1e020000005fa3"), ("expect_swing_lr", "0"), ("expect_swing_ud", "0")], "test_properties_parsing; 0x0009/0x000A, includes unsupported INDOOR_HUMIDITY")
r = Response.construct(bytes.fromhex("aa18ac00000000000302b0020a0000013209001101000089a4"))
assert type(r) is PropertiesResponse
lines += vec("props_ack", [("frame", "aa18ac00000000000302b0020a0000013209001101000089a4"), ("expect_swing_lr", "50"), ("expect_swing_ud", "0")], "test_properties_ack; SWING_UD failed result 0x11")
r = Response.construct(bytes.fromhex("aa1aac00000000000205b50310060101090001010a000101dcbcb4"))
assert type(r) is Response
lines += vec("props_notify", [("frame", "aa1aac00000000000205b50310060101090001010a000101dcbcb4")], "test_properties_notify; decodes to generic Response (ignored)")
r = Response.construct(bytes.fromhex("aa1bac00000000000202b0021e001004001000001a00000100000e18"))
assert type(r) is PropertiesResponse
lines += vec("props_unknown", [("frame", "aa1bac00000000000202b0021e001004001000001a00000100000e18")], "test_properties_unknown_and_invalid; unknown ID 0x001E, BUZZER not decoded")
r = Response.construct(bytes.fromhex("aa18ac00000000000302b00243001101041a00000100002ce5"))
assert type(r) is PropertiesResponse
lines += vec("props_exec_failed", [("frame", "aa18ac00000000000302b00243001101041a00000100002ce5")], "test_properties_execution_failed; BREEZE_CONTROL failed 0x11")
try:
    Response.construct(bytes.fromhex("aa14ac00000000000303b10109000001003c0000FF"))
    raise SystemExit("expected InvalidFrameException")
except InvalidFrameException:
    pass
lines += vec("frame_bad_checksum", [("frame", "aa14ac00000000000303b10109000001003c0000FF"), ("expect", "InvalidFrameException")], "test_invalid_checksum; frame csum 0xFF invalid")
r = Response.construct(bytes.fromhex("aa14ac00000000000303b10109000001003c000042"))
assert type(r) is PropertiesResponse
lines += vec("props_bad_crc", [("frame", "aa14ac00000000000303b10109000001003c000042")], "test_properties_response_invalid_crc; bad payload CRC still accepted for properties")
try:
    Response.construct(bytes.fromhex("aa22ac00000000000303c0014566000000300010045eff00000000000000000069aa0c"))
    raise SystemExit("expected InvalidResponseException")
except InvalidResponseException:
    pass
lines += vec("state_bad_crc", [("frame", "aa22ac00000000000303c0014566000000300010045eff00000000000000000069aa0c"), ("expect", "InvalidResponseException")], "test_properties_response_invalid_crc; state rejects bad crc")
try:
    Response.construct(bytes.fromhex("01000000"))
    raise SystemExit("expected InvalidFrameException")
except InvalidFrameException:
    pass
lines += vec("frame_short", [("frame", "01000000"), ("expect", "InvalidFrameException")], "test_short_packet")
CC_FRAME = "aa63cc0000000000000301fe00000043005000728c8000bc00728c728c808000010141ff010203000603010000000000000001000103010000000000000000000001000100010000000000000000000000000001000200000100000101000102ff02ffa2"
in_src(CC_FRAME, SRC_CMD_TEXT)
try:
    Response.construct(bytes.fromhex(CC_FRAME))
    raise SystemExit("expected InvalidFrameException")
except InvalidFrameException:
    pass
lines += vec("frame_wrong_device_type", [("frame", CC_FRAME), ("expect", "InvalidFrameException")], "test_invalid_device_type; CC (0xCC) device frame")
write("properties.txt", lines)

# ---------------------------------------------------------------- group.txt
print("== group vectors")
lines = hdr("Group data response vectors", "msmart/device/AC/test_command.py TestGroupDataResponse")
def group4(hexstr, total, cur, rt, binary=False):
    in_src(hexstr, SRC_CMD_TEXT)
    r = Response.construct(bytes.fromhex(hexstr))
    assert type(r) is Group4Response, hexstr
    if binary:
        assert r.total_energy_binary == total and r.current_energy_binary == cur and r.real_time_power_binary == rt
    else:
        assert r.total_energy == total and r.current_energy == cur and r.real_time_power == rt

group4("aa1fac00000000000303c121014400067920000000000000000000000000aabf", 679.2, 0, 0)
lines += vec("energy_1", [("frame", "aa1fac00000000000303c121014400067920000000000000000000000000aabf"), ("expect_total", "679.2"), ("expect_current", "0"), ("expect_realtime", "0")], "test_energy_usage")
group4("aa20ac00000000000203c121014400564a02640000000014ae0000000000041a22", 5650.02, 1514.0, 0)
lines += vec("energy_2", [("frame", "aa20ac00000000000203c121014400564a02640000000014ae0000000000041a22"), ("expect_total", "5650.02"), ("expect_current", "1514.0"), ("expect_realtime", "0")], "test_energy_usage")
group4("aa20ac00000000000303c1210144000000000000000000000000000000000843bc", None, None, None)
lines += vec("energy_none", [("frame", "aa20ac00000000000303c1210144000000000000000000000000000000000843bc"), ("expect_total", "none"), ("expect_current", "none"), ("expect_realtime", "none")], "test_energy_usage; zeros -> None")
group4("aa22ac00000000000803c1210144000005e00000000000000006000aeb000000487a5e", 150.4, 0.6, 279.5, binary=True)
lines += vec("energy_binary", [("frame", "aa22ac00000000000803c1210144000005e00000000000000006000aeb000000487a5e"), ("expect_total", "150.4"), ("expect_current", "0.6"), ("expect_realtime", "279.5")], "test_binary_energy_usage")
r = Response.construct(bytes.fromhex("aa20ac00000000000303c12101453f546c005d0a000000de1f0000ba9a0004af9c"))
assert type(r) is Group5Response and r.humidity == 63
lines += vec("humidity", [("frame", "aa20ac00000000000303c12101453f546c005d0a000000de1f0000ba9a0004af9c"), ("expect_humidity", "63")], "test_humidity")
r = Response.construct(bytes.fromhex("aa1fac00000000000303c1210145000000000000000000000000000000001aed"))
assert type(r) is Group5Response and r.humidity is None
lines += vec("humidity_none", [("frame", "aa1fac00000000000303c1210145000000000000000000000000000000001aed"), ("expect_humidity", "none")], "test_humidity; unsupported")
d = Group5Response(memoryview(bytes.fromhex("c12101451e4f2b5e003c01000000692900cf7e0001bb38")))
assert d.defrost is True
lines += vec("defrost_true", [("payload", "c12101451e4f2b5e003c01000000692900cf7e0001bb38"), ("expect_defrost", "1")], "test_defrost; raw Group5 payload")
d = Group5Response(memoryview(bytes.fromhex("c12101451e4dcf5e611f00000052ad2900cf7e0002")))
assert d.defrost is False
lines += vec("defrost_false", [("payload", "c12101451e4dcf5e611f00000052ad2900cf7e0002"), ("expect_defrost", "0")], "test_defrost")
g = Group1Response(memoryview(bytes.fromhex("c10000411c1d0001e800472666582d0000000000")))
assert g.compressor_frequency == 28 and g.target_compressor_frequency == 29 and g.compressor_current == 1 and g.compressor_voltage == 232
assert g.indoor_temperature == 20.5 and g.indoor_coil_temperature == 4.0 and g.outdoor_coil_temperature == 26.0 and g.outdoor_temperature == 19.0 and g.discharge_pipe_temperature == 45
lines += vec("group1", [("payload", "c10000411c1d0001e800472666582d0000000000"), ("expect_freq", "28"), ("expect_target_freq", "29"), ("expect_current", "1"), ("expect_voltage", "232"), ("expect_indoor", "20.5"), ("expect_indoor_coil", "4.0"), ("expect_outdoor_coil", "26.0"), ("expect_outdoor", "19.0"), ("expect_tp", "45")], "test_group1_response")
g = Group2Response(memoryview(bytes.fromhex("c100004234350000100000000000000000000000")))
assert g.target_indoor_fan_speed == 416 and g.indoor_fan_speed == 424 and g.water_pump_running is True
lines += vec("group2", [("payload", "c100004234350000100000000000000000000000"), ("expect_target_fan", "416"), ("expect_fan", "424"), ("expect_pump", "1")], "test_group2_response")
g = Group7Response(memoryview(bytes.fromhex("c10000470000000000000d010000000000000000")))
assert g.outdoor_unit_power == 269
lines += vec("group7", [("payload", "c10000470000000000000d010000000000000000"), ("expect_power", "269")], "test_group7_response; payload[10]+256*payload[11]")
g = Group11Response(memoryview(bytes.fromhex("c100004b0064006400486400f000000000000000")))
assert g.horizontal_louvers_angle == 72 and g.vertical_louvers_angle == 240
lines += vec("group11", [("payload", "c100004b0064006400486400f000000000000000"), ("expect_h_louvers", "72"), ("expect_v_louvers", "240")], "test_group11_response")
write("group.txt", lines)

# ---------------------------------------------------------------- commands.txt
print("== commands vectors")
lines = hdr("Command builder frame vectors", "msmart/device/AC/command.py:185-379 (Command/GetState/SetState)")


def cmd_frame(counter, build):
    """Build a frame with Command._message_id pinned so encode is deterministic."""
    Command._message_id = counter
    frame = build()
    assert frame[0] == 0xAA and frame[2] == 0xAC
    assert frame[1] == len(frame) - 1, "length byte"
    Frame.validate(memoryview(frame), DeviceType.AIR_CONDITIONER)
    assert frame[-3] == (counter + 1) & 0xFF, "message id byte (payload, crc8, checksum from end)"
    return frame


def gs_vec(name, counter, temp_type, comment):
    def build():
        c = GetStateCommand()
        c.temperature_type = temp_type
        return c.tobytes()
    f = cmd_frame(counter, build)
    assert f[17] == temp_type, "temperature_type position"
    lines.extend(vec(name, [
        ("temperature_type", "%02x" % temp_type),
        ("message_id_counter", "%02x" % counter),
        ("frame", f.hex()),
    ], comment))


gs_vec("getstate_indoor", 0x10, 0x2, "default temperature_type INDOOR; same counter as frame.txt getstate_full")
assert cmd_frame(0x10, GetStateCommand().tobytes)[10:-1].hex() == GETSTATE_PAYLOAD
gs_vec("getstate_outdoor", 0x20, 0x3, "temperature_type OUTDOOR")


SETSTATE_ORDER = [
    "beep_on", "power_on", "target_temperature", "operational_mode", "fan_speed",
    "eco", "swing_mode", "turbo", "fahrenheit", "sleep", "freeze_protection",
    "follow_me", "purifier", "target_humidity", "aux_heat", "force_aux_heat",
    "independent_aux_heat",
]
SETSTATE_DEFAULTS = {
    "beep_on": True, "power_on": False, "target_temperature": 25.0, "operational_mode": 0,
    "fan_speed": 0, "eco": True, "swing_mode": 0, "turbo": False, "fahrenheit": True,
    "sleep": False, "freeze_protection": False, "follow_me": False, "purifier": False,
    "target_humidity": 40, "aux_heat": False, "force_aux_heat": False,
    "independent_aux_heat": False,
}
assert len(SETSTATE_ORDER) == 17
assert set(SETSTATE_DEFAULTS) == set(SETSTATE_ORDER)


def ss_vec(name, counter, comment, byte_checks=(), **kw):
    p = dict(SETSTATE_DEFAULTS)
    p.update(kw)

    def build():
        c = SetStateCommand()
        for k, v in p.items():
            assert hasattr(c, k), k
            setattr(c, k, v)
        return c.tobytes()

    f = cmd_frame(counter, build)
    assert len(f) == 37, "SetState frame length"  # 10 hdr + 24 data + id + crc8 + csum
    for idx, val in byte_checks:  # data byte N (0-based) -> expected value, documents encoding
        assert f[10 + idx] == val, "%s byte %d = 0x%02X, expected 0x%02X" % (name, idx, f[10 + idx], val)
    fields = [("message_id_counter", "%02x" % counter)]
    for k in SETSTATE_ORDER:
        v = p[k]
        fields.append((k, ("1" if v else "0") if isinstance(v, bool) else repr(v) if isinstance(v, float) else str(v)))
    fields.append(("frame", f.hex()))
    lines.extend(vec(name, fields, comment))


ss_vec("setstate_defaults", 0x01, "Python defaults: beep+eco+fahrenheit on, target 25.0, humidity 40",
       byte_checks=[(1, 0x42), (2, 0x09), (10, 0x04)])  # byte1=SRC|beep, byte2=25-16, byte10=fahrenheit
ss_vec("setstate_cool_half", 0x11, "power on, 24.5 C (half-degree bit), mode 3, fan 50",
       byte_checks=[(1, 0x43), (2, 0x78), (3, 50)],  # byte2=0x10|8|(3<<5)
       power_on=True, target_temperature=24.5, operational_mode=3, fan_speed=50)
ss_vec("setstate_boundary_17", 0x22, "primary range lower boundary 17.0",
       byte_checks=[(2, 0x01), (18, 0x00)], target_temperature=17.0)
ss_vec("setstate_boundary_30", 0x33, "primary range upper boundary 30.0",
       byte_checks=[(2, 0x0E), (18, 0x00)], target_temperature=30.0)
ss_vec("setstate_30_5", 0x44, "30.5 still primary (int part in range) + half-degree bit",
       byte_checks=[(2, 0x1E), (18, 0x00)], target_temperature=30.5)
ss_vec("setstate_below_17", 0x55, "16.0 -> alternate encoding, temperature_alt=(16-12)&0x1F=4",
       byte_checks=[(2, 0x00), (18, 0x04)], target_temperature=16.0)
ss_vec("setstate_16_5", 0x66, "16.5 -> alternate + half-degree bit on zero primary temp",
       byte_checks=[(2, 0x10), (18, 0x04)], target_temperature=16.5)
ss_vec("setstate_above_30", 0x77, "31.5 -> alternate (31-12)&0x1F=19 + half-degree bit",
       byte_checks=[(2, 0x10), (18, 0x13)], target_temperature=31.5)
ss_vec("setstate_negative", 0x88, "-5.5 -> alt (-5-12)&0x1F=15 (two's-complement &), frac<0 sets no half bit",
       byte_checks=[(2, 0x00), (18, 0x0F)], target_temperature=-5.5)
ss_vec("setstate_all_flags", 0x99, "every flag on except beep/eco/fahrenheit; mode 0x0A masked to 2; humidity raw 100",
       byte_checks=[(1, 0x03), (2, 0x49), (7, 0x33), (8, 0xA0), (9, 0x38), (10, 0x03), (21, 0x80), (22, 0x08)],
       beep_on=False, power_on=True, operational_mode=0x0A, fan_speed=191, eco=False, swing_mode=3,
       turbo=True, fahrenheit=False, sleep=True, freeze_protection=True, follow_me=True, purifier=True,
       target_humidity=100, aux_heat=True, force_aux_heat=True, independent_aux_heat=True)
ss_vec("setstate_humidity_mask", 0xAA, "target_humidity 0xFF masked to 0x7F",
       byte_checks=[(19, 0x7F)], target_humidity=0xFF)
ss_vec("setstate_id_wrap", 0xFF, "message id counter wraps: 0xFF + 1 & 0xFF = 0x00 in payload", byte_checks=[])


def simple_cmd_vec(name, counter, build, command, extra, comment, frame_len):
    f = cmd_frame(counter, build)
    assert len(f) == frame_len, (name, len(f))
    lines.extend(vec(name, [("command", command), ("message_id_counter", "%02x" % counter)] + extra + [("frame", f.hex())], comment))


simple_cmd_vec("getcapabilities", 0x01, lambda: GetCapabilitiesCommand().tobytes(), "get_capabilities", [("additional", "0")], "GetCapabilitiesCommand default", 16)
simple_cmd_vec("getcapabilities_additional", 0x02, lambda: GetCapabilitiesCommand(True).tobytes(), "get_capabilities", [("additional", "1")], "GetCapabilitiesCommand additional page", 17)
simple_cmd_vec("toggledisplay", 0x03, lambda: ToggleDisplayCommand().tobytes(), "toggle_display", [("beep_on", "1")], "ToggleDisplayCommand default (beep on); QUERY frame type", 34)
for _g in (1, 11):
    simple_cmd_vec("getgroupdata_g%d" % _g, 0x40 | _g, lambda g=_g: GetGroupDataCommand(g).tobytes(), "get_group_data", [("group", str(_g))], "GetGroupDataCommand group %d: payload 41 21 01 (40|group) + 16 zero bytes; QUERY frame" % _g, 33)
def _toggle_mute():
    c = ToggleDisplayCommand()
    c.beep_on = False
    return c.tobytes()


simple_cmd_vec("toggledisplay_mute", 0x04, _toggle_mute, "toggle_display", [("beep_on", "0")], "ToggleDisplayCommand beep_off", 34)

write("commands.txt", lines)

# ---------------------------------------------------------------- discovery.txt
print("== discovery vectors")
import asyncio
import re

from msmart.const import DISCOVERY_MSG
from msmart.discover import Discover, DiscoverError

SRC_DISC_TEXT = SRC_DISC.read_text()
SRC_DISCOVER_TEXT = (MSMART / "msmart" / "discover.py").read_text()
assert "discovery_packets: int = 3" in SRC_DISCOVER_TEXT, "discovery default packet count moved"
assert "[6445, 20086]" in SRC_DISCOVER_TEXT, "discovery ports moved"

DISC_HEXES = [h for h in re.findall(r'bytes\.fromhex\("([0-9a-f]+)"\)', SRC_DISC_TEXT) if len(h) >= 200]
DISC_IPS = re.findall(r'\("(\d+\.\d+\.\d+\.\d+)", 6445\)', SRC_DISC_TEXT)
assert len(DISC_HEXES) == 2 and len(DISC_IPS) == 2, (DISC_IPS, len(DISC_HEXES))

# Expectations copied verbatim from test_discover.py assertions.
DISC_EXPECT = [
    ("v2", "2", "15393162840672", "6444", "net_ac_F7B4", "000000P0000000Q1F0C9D153F7B40000"),
    ("v3", "3", "147334558165565", "6444", "net_ac_63BA", "000000P0000000Q1B88C29C963BA0000"),
]
for _tag, exp_ver, exp_id, exp_port, exp_name, exp_sn in DISC_EXPECT:
    assert f"self.assertEqual(version, {exp_ver})" in SRC_DISC_TEXT
    assert f'self.assertEqual(info["device_id"], {exp_id})' in SRC_DISC_TEXT
    assert f'self.assertEqual(info["port"], {exp_port})' in SRC_DISC_TEXT
    assert f'self.assertEqual(info["name"], "{exp_name}")' in SRC_DISC_TEXT
    assert f'self.assertEqual(info["sn"], "{exp_sn}")' in SRC_DISC_TEXT


def disc_view(data, version):
    # _get_device_info: V3 strips [8:-16] first; V2 keeps the whole datagram.
    return data[8:len(data) - 16] if version == 3 else data


def disc_enc(data, version):
    v = disc_view(data, version)
    return v[40:len(v) - 16]


def disc_raises(fn):
    try:
        fn()
    except Exception:
        return True
    return False


disc_lines = []
V2_FRAME = None
V2_DEC = None
for (ip, hexstr), (_tag, exp_ver, exp_id, exp_port, exp_name, exp_sn) in zip(zip(DISC_IPS, DISC_HEXES), DISC_EXPECT):
    data = bytes.fromhex(hexstr)
    version = Discover._get_device_version(data)
    assert version == int(exp_ver), (ip, version)
    info = asyncio.run(Discover._get_device_info(ip, version, data))
    assert info["device_id"] == int(exp_id), info
    assert info["port"] == int(exp_port), info
    assert info["name"] == exp_name and info["sn"] == exp_sn, info
    assert info["device_type"] == DeviceType.AIR_CONDITIONER, info
    enc = disc_enc(data, version)
    dec = Security.decrypt_aes(enc)
    assert len(dec) >= 41 and dec[40] == len(exp_name), (len(dec), dec[40])
    assert bytes(reversed(dec[0:4])) == bytes(int(p) for p in ip.split(".")), "ip octets reversed"
    assert dec[8:40].decode() == exp_sn
    assert int.from_bytes(disc_view(data, version)[20:26], "little") == info["device_id"]
    assert len(enc) % 16 == 0
    disc_lines.extend(vec(_tag + "_response", [
        ("frame", hexstr), ("source_ip", ip), ("version", exp_ver),
        ("encrypted", enc.hex()), ("decrypted", dec.hex()),
        ("device_id", exp_id), ("port", exp_port),
        ("name", exp_name), ("sn", exp_sn),
        ("device_type", "%x" % info["device_type"]),
    ], "test_discover.py V%d golden response parsed by Discover._get_device_info" % version))
    if version == 2:
        V2_FRAME, V2_DEC = data, dec
assert V2_FRAME is not None and V2_DEC is not None


def rebuild_v2(new_enc):
    return V2_FRAME[:40] + new_enc + V2_FRAME[-16:]


disc_lines.extend(vec("discovery_msg", [
    ("msg", DISCOVERY_MSG.hex()),
    ("port_1", "6445"), ("port_2", "20086"),
    ("default_packets", "3"),
    ("broadcast", "255.255.255.255"),
], "const.py DISCOVERY_MSG; _send_discovery targets both ports x discovery_packets"))

XML_FRAME = b'<?xml version="1.0" encoding="utf-8"?><html></html>'
assert Discover._get_device_version(XML_FRAME) == 1, "V1 XML detection changed"
assert disc_raises(lambda: Discover._get_device_version(b"\x00\x01"))
assert disc_raises(lambda: Discover._get_device_version(b"\x5a"))
disc_lines.extend(vec("xml_response", [("frame", XML_FRAME.hex()), ("version", "0")],
                      "V1 XML probe: Python version=1 (unsupported), C++ maps to kUnsupported"))
disc_lines.extend(vec("prefix_unknown", [("frame", "0001"), ("version", "0")], "neither 5a5a nor 8370 prefix"))
disc_lines.extend(vec("single_byte", [("frame", "5a"), ("version", "0")], "len<2 cannot classify"))

TRUNC = V2_FRAME[:55]
assert disc_raises(lambda: asyncio.run(Discover._get_device_info("10.0.0.1", 2, TRUNC)))
disc_lines.extend(vec("v2_truncated", [("frame", TRUNC.hex()), ("error", "decrypt")],
                      "view too short for the encrypted region; upstream decrypt error too"))

BAD_PAD = bytearray(Security.encrypt_aes(V2_DEC))
BAD_PAD[-1] ^= 0xFF
BAD_FRAME = rebuild_v2(bytes(BAD_PAD))
assert disc_raises(lambda: asyncio.run(Discover._get_device_info("10.0.0.1", 2, BAD_FRAME)))
disc_lines.extend(vec("v2_bad_padding", [("frame", BAD_FRAME.hex()), ("error", "decrypt")],
                      "last encrypted byte flipped -> PKCS7 unpad fails"))

BAD_NAME_FRAME = rebuild_v2(Security.encrypt_aes(V2_DEC[:40] + bytes([len(b"nocookie")]) + b"nocookie"))
assert disc_raises(lambda: asyncio.run(Discover._get_device_info("10.0.0.1", 2, BAD_NAME_FRAME)))
disc_lines.extend(vec("v2_bad_name", [("frame", BAD_NAME_FRAME.hex()), ("error", "name")],
                      "name without '_<hex>' segment: upstream IndexError, C++ kBadName"))

LONG_NAME = b"net_ac_" + b"4" * 40
LONG_FRAME = rebuild_v2(Security.encrypt_aes(V2_DEC[:40] + bytes([len(LONG_NAME)]) + LONG_NAME))
LONG_INFO = asyncio.run(Discover._get_device_info("10.0.0.1", 2, LONG_FRAME))
assert LONG_INFO["name"] == LONG_NAME.decode()
disc_lines.extend(vec("v2_long_name", [("frame", LONG_FRAME.hex()), ("error", "name_too_long")],
                      "47-byte name parses upstream but exceeds the C++ name capacity cap (hardening)"))

write("discovery.txt", hdr(
    "Discovery message + V2/V3 response parsing",
    "msmart/const.py::DISCOVERY_MSG, msmart/discover.py::_get_device_version/_get_device_info, msmart/tests/test_discover.py::_DISCOVER_RESPONSES") + disc_lines)

print("ALL VECTORS EXTRACTED AND VERIFIED")
