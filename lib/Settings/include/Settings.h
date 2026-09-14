#pragma once

// Freedea settings core: plain-data struct plus JSON schema encode/decode.
// Pure C++ (ArduinoJson's portable header) so it is host-unit-testable under
// test/host/; SD-file glue lives in the firmware's src/SettingsStore.

#include <cstddef>
#include <cstdint>

namespace settings {

// Bump on every change to the on-SD JSON shape; readers reject a mismatch
// rather than misinterpreting fields. v2 added the weather block; v3 replaced
// the single ssid/password with multi-network profiles, an active-profile
// pointer and a per-profile captive-portal cache (7.2); v4 added the
// WireGuard block (7.3); v5 added the AC block (8.8/8.9). Readers still
// accept v1/v2 payloads (JSON and binary) and migrate the stored pair into
// profile 0, so an update never loses the WiFi credentials.
constexpr uint16_t kSchemaVersion = 5;
constexpr uint16_t kMinSupportedSchemaVersion = 1;

// 802.11 SSID is at most 32 octets; WPA2 passphrase at most 63 chars.
constexpr size_t kSsidMaxLen = 32;
constexpr size_t kPasswordMaxLen = 64;

// Saved WiFi networks. Fixed-size so the binary store stays a fixed-size
// snapshot and the profile list never touches the heap.
constexpr uint8_t kMaxNetworks = 4;

// Settings::activeNetwork value when no profile owns the STA connection.
constexpr uint8_t kNoNetwork = 0xFF;

// Captive-portal probe cache (7.2c), per profile: probed once after the
// network is first joined and stored in the profile. Editing the network in
// the portal resets it to unknown.
constexpr uint8_t kPortalUnknown = 0; // never probed
constexpr uint8_t kPortalOpen = 1;    // plain internet, no login page
constexpr uint8_t kPortalFound = 2;   // captive login page intercepts HTTP

// Weather location display name (the Dashboard panel's left label).
constexpr size_t kWeatherNameMaxLen = 16;

// Coordinates are fixed-point 1e-4 degrees: int32 covers ±90°/±180° with
// ~11 m resolution, keeps floats out of the binary serialization, and stays
// inside float32 round-trip precision in the JSON surface (ArduinoJson
// stores JSON fractions as float, which cannot carry 8 significant digits —
// microdegrees came back two off).
constexpr int32_t kLatMaxE4 = 900000;
constexpr int32_t kLonMaxE4 = 1800000;

struct Network {
  char ssid[kSsidMaxLen + 1] = {};
  char password[kPasswordMaxLen + 1] = {};
  uint8_t portalState = kPortalUnknown;
};

struct Weather {
  bool enabled = false;
  char name[kWeatherNameMaxLen + 1] = {};
  // (0, 0) means "not configured" — weatherConfigured() gates on it.
  int32_t latE4 = 0;
  int32_t lonE4 = 0;
};

// WireGuard client (7.3). Single peer, reference-C crypto; the endpoint may
// be a hostname or a dotted-quad. Keys are WireGuard base64 (44 chars
// encoding 32 bytes, always padded with '='). Fixed-size so the binary
// store stays a fixed snapshot; ownIp is the tunnel IPv4 as text (parsed
// at use, kept verbatim here).
constexpr size_t kWgEndpointMaxLen = 64;
constexpr size_t kWgKeyLen = 44;
constexpr size_t kWgIpMaxLen = 15; // "255.255.255.255"
constexpr uint16_t kWgKeepaliveMaxSecs = 300;
constexpr uint16_t kWgKeepaliveDefaultSecs = 25;

struct WireGuard {
  bool enabled = false;
  char endpoint[kWgEndpointMaxLen + 1] = {};
  uint16_t port = 0; // UDP port on the peer (also our listen port)
  char ownPrivateKey[kWgKeyLen + 1] = {};
  char peerPublicKey[kWgKeyLen + 1] = {};
  char ownIp[kWgIpMaxLen + 1] = {};
  // Persistent keepalive seconds; 0 keeps the library default (10 s).
  // The radio wakes on this interval, so the default is raised (5.4).
  uint16_t keepalive = kWgKeepaliveDefaultSecs;
};

// AC behavior flags kept device-side (not AC state). beep drives the BUZZER
// property and the beep bit of our commands (8.8); buttonLock arms the
// monitor-&-override guard (8.9).
struct Ac {
  bool beep = true;
  bool buttonLock = false;
};

struct Settings {
  uint16_t schemaVersion = kSchemaVersion;
  Network networks[kMaxNetworks];
  // Profile the STA connects to, or kNoNetwork. Invariant: an index below
  // kMaxNetworks always names a slot with a non-empty SSID.
  uint8_t activeNetwork = kNoNetwork;
  Weather weather;
  WireGuard wireguard;
  Ac ac;
};

// Profile the STA connects to: null when activeNetwork is kNoNetwork or
// names an empty slot.
const Network* activeNetwork(const Settings& s);

// True when at least one profile carries a non-empty SSID.
bool anyNetwork(const Settings& s);

// True when both coordinates are set and in range. Independent of the
// enabled flag: a location can be stored while weather is switched off.
bool weatherConfigured(const Weather& w);

// True when every WireGuard field a tunnel needs is present (endpoint, port
// in 1..65535, both full-length keys, tunnel IP). Independent of the enabled
// flag: a config can be stored while WireGuard is switched off.
bool wireGuardConfigured(const WireGuard& w);

// Upper bound on the serialized size: JSON escaping inflates each source byte
// to at most \uXXXX (6 bytes); the rest covers keys, array punctuation, braces
// and the NUL.
constexpr size_t kSerializedMaxSize = 6 * (kMaxNetworks * (kSsidMaxLen + kPasswordMaxLen) + kWeatherNameMaxLen +
                                           kWgEndpointMaxLen + 2 * kWgKeyLen + kWgIpMaxLen) +
                                      400;

// Writes `s` as a JSON object into buf, always NUL-terminated.
// Returns the byte count (excluding NUL), or 0 if it does not fit.
size_t serialize(const Settings& s, char* buf, size_t cap);

// Parses a JSON object into `out`. Absent keys keep the value already in
// `out`, except a present "networks" array, which replaces the whole profile
// list. Returns false — leaving `out` untouched — on parse error, non-object
// input, a schema-version mismatch, or any mistyped/oversized field. A v1/v2
// document (top-level ssid/pass) migrates into profile 0.
bool deserialize(const char* json, Settings& out);

// Binary payload for the encrypted settings.bin store (3.4). Fixed size
// (the Vault envelope zero-pads to the AES block anyway). v1 (112 B): u16
// schema version LE, ssid[33], password[65], 12 reserved bytes. v2 (144 B)
// replaces the reserved run with the weather block — enabled u8 (0/1),
// name[17], latE4 i32 LE, lonE4 i32 LE. v3 (432 B): multi-network layout —
// version u16 LE, activeIndex u8, reserved u8, then kMaxNetworks slots of
// ssid[33] + password[65] + portalState u8, then the weather block at 400.
// v4 (608 B): appends the WireGuard block at 432 — enabled u8 (0/1),
// endpoint[65], port u16 LE, ownPrivateKey[45], peerPublicKey[45],
// ownIp[16], keepalive u16 LE. v5 (610 B): appends the AC block at 608 —
// beep u8 (0/1), buttonLock u8 (0/1). Version and length stay bound 1:1
// (112/144/432/608/610).
constexpr size_t kBinPayloadSizeV1 = 112;
constexpr size_t kBinPayloadSizeV2 = 144;
constexpr size_t kBinPayloadSizeV3 = 432;
constexpr size_t kBinPayloadSizeV4 = 608;
constexpr size_t kBinPayloadSize = 610;

// Writes exactly kBinPayloadSize bytes (always the v3 layout).
void serializeBin(const Settings& s, uint8_t* out);

// Strict full-snapshot parse: exactly kBinPayloadSize bytes with version 5,
// or the legacy lengths 608/432/144/112 with versions 4/3/2/1 (credentials
// migrate to profile 0, activeNetwork becomes 0 when the SSID is set; v3 and
// older leave the WireGuard block at its defaults, v4 and older the AC
// block); all strings
// NUL-terminated, portal state and coordinates in range, on any failure
// `out` is left untouched. Unlike the JSON path this never merges — the
// binary payload is the complete state. A parsed payload is tagged as the
// current version, so the next save upgrades the store.
bool deserializeBin(const uint8_t* in, size_t len, Settings& out);

} // namespace settings
