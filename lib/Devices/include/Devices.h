#pragma once

// Midea AC device records (3.4): fixed-size binary payload for the encrypted
// devices.bin store, plus the hand-editable device.json import parser with
// strict hex validation. The normal boot path uses only the binary codec (no
// JSON parsing); ArduinoJson lives on the import surface.

#include <cstddef>
#include <cstdint>

namespace devices {

constexpr size_t kMaxDevices = 4;
constexpr size_t kNameCapacity = 24; // display name: 23 chars + NUL
constexpr size_t kTokenMaxLen = 128; // raw V3 token bytes (cloud blob)
constexpr size_t kKeyLen = 32;       // V3 handshake key (AES-256 material)
constexpr uint16_t kDefaultPort = 6444;
constexpr uint16_t kPayloadVersion = 1;

struct Device {
  uint64_t id = 0;                  // 48-bit msmart device id
  uint16_t port = kDefaultPort;     // TCP control port
  uint8_t version = 0;              // protocol version: 0 unknown, 2, 3
  uint8_t ip[4] = {};               // last known IPv4; all-zero = unknown
  char name[kNameCapacity] = {};    // ASCII display name, NUL-terminated
  uint8_t token[kTokenMaxLen] = {}; // V3 auth token (with key)
  uint16_t tokenLen = 0;            // 0 = unauthenticated device (V1/V2)
  uint8_t key[kKeyLen] = {};        // V3 auth key

  bool hasCredentials() const { return tokenLen > 0; }
};

struct List {
  Device devices[kMaxDevices];
  uint8_t count = 0;
};

// --- binary payload (counterpart of settings' serializeBin) -------------------
// Fixed size, little-endian, all multi-byte fields byte-copied (never cast):
//   u16 version | u16 count | count records of kRecordSize:
//     u64 id | u8 version | u8 rsv | u16 port | ip[4] | name[24]
//     u16 tokenLen | key[32] | token[128] | zero padding
// Records beyond `count` are fully zeroed.
constexpr size_t kRecordSize = 208;
constexpr size_t kBinPayloadSize = 4 + kMaxDevices * kRecordSize;

// Writes exactly kBinPayloadSize bytes.
void serializeBin(const List& list, uint8_t* out);

// Strict parse: exact length, matching payload version, count <= max,
// tokenLen <= max (and 0 for slots beyond count), NUL-terminated names;
// `out` untouched on failure.
bool deserializeBin(const uint8_t* in, size_t len, List& out);

// --- SD import file -----------------------------------------------------------

// Failure causes (logged for the user; they never carry credential material).
enum class ImportError : uint8_t {
  kNone = 0,
  kJson,       // unparsable, not an object, or missing/non-array "devices"
  kVersion,    // missing or unsupported "v"
  kTooMany,    // more than kMaxDevices entries
  kBadId,      // missing, zero, non-decimal, or duplicate "id"
  kBadName,    // non-string, non-printable-ASCII, or too long
  kBadVersion, // "version" present but not 2 or 3
  kBadIp,      // malformed dotted quad
  kBadPort,    // present but outside 1..65535
  kBadToken,   // bad hex/oversized/empty, or key given without a token
  kBadKey,     // token given without a key, or key hex invalid / not 32 B
};

// Parses the hand-editable /.freedea/device.json:
//   {"v":1,"devices":[{"id":15393162840672,"name":"Living room",
//     "version":3,"ip":"192.168.1.50","port":6444,
//     "token":"<even hex, <=256 chars>","key":"<64 hex chars>"}]}
// id: JSON number or decimal/0x string, non-zero, unique. Defaults: name
// "AC-<last 4 hex of id>", port 6444, version 0, no ip. token+key are
// required together and optional together (V1/V2 devices have none).
// `out` is replaced only when the whole document parses cleanly.
ImportError parseImportJson(const char* json, List& out);

} // namespace devices
