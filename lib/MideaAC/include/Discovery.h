#pragma once

// Midea LAN discovery: the const.py DISCOVERY_MSG broadcast payload plus the
// V2/V3 discovery-response parser (discover.py _get_device_version and
// _get_device_info, 247-353 at d7db53b). UDP send/receive is Phase 3
// transport; this module is message constants and pure response parsing.
// V1 (XML) devices are unsupported upstream; here they are not distinguished
// from other garbage and classify as kUnsupported.

#include <cstddef>
#include <cstdint>

namespace midea {

// const.py DISCOVERY_MSG (64 bytes), broadcast to both ports below.
inline constexpr uint8_t kDiscoveryMessage[] = {
    0x5a, 0x5a, 0x01, 0x11, 0x48, 0x00, 0x92, 0x00, //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //
    0x7f, 0x75, 0xbd, 0x6b, 0x3e, 0x4f, 0x8b, 0x76, //
    0x2e, 0x84, 0x9c, 0x6e, 0x57, 0x8d, 0x65, 0x90, //
    0x03, 0x6e, 0x9d, 0x43, 0x42, 0xa5, 0x0f, 0x1f, //
    0x56, 0x9e, 0xb8, 0xec, 0x91, 0x8e, 0x92, 0xe5, //
};
inline constexpr size_t kDiscoveryMessageLen = sizeof(kDiscoveryMessage);

// _send_discovery (discover.py:93-99) sends every packet to both ports,
// repeated kDiscoveryDefaultPackets times, to kDiscoveryBroadcast or a
// single target host.
inline constexpr uint16_t kDiscoveryPortPrimary = 6445;
inline constexpr uint16_t kDiscoveryPortSecondary = 20086;
inline constexpr unsigned kDiscoveryDefaultPackets = 3;
inline constexpr char kDiscoveryBroadcast[] = "255.255.255.255";

// Response class from the first two bytes: 5A5A = V2, 8370 = V3
// (discover.py _get_device_version; the XML/V1 probe collapses into
// kUnsupported).
enum class DiscoveryVersion : uint8_t {
  kUnsupported = 0,
  kV2 = 2,
  kV3 = 3,
};

DiscoveryVersion getDiscoveryVersion(const uint8_t* data, size_t len);

// Cap on the AES-decrypted discovery struct. The observed V3 payload
// decrypts to 122 bytes; larger is rejected so the parser keeps one fixed
// 160-byte stack buffer.
inline constexpr size_t kDiscoveryMaxDecrypted = 160;

// Fixed capacity for the name/SSID field ("net_ac_XXXX" is 11 chars).
inline constexpr size_t kDiscoveryNameCapacity = 40;

// Fields parsed from the AES-ECB-encrypted discovery struct. ip holds the
// *received* (source) address: upstream parses the embedded IP only to warn
// on mismatch and reports the received one (discover.py:334-343).
struct DiscoveryDeviceInfo {
  uint64_t deviceId = 0;   // little-endian, 6 bytes at view[20:26]
  uint16_t port = 0;       // TCP control port (6444 on observed devices)
  uint16_t deviceType = 0; // hex segment of name: "net_ac_F7B4" -> 0xAC
  DiscoveryVersion version = DiscoveryVersion::kUnsupported;
  char sn[33] = {};                       // fixed 32-byte serial field
  char name[kDiscoveryNameCapacity] = {}; // null-terminated SSID
  char ip[16] = {};                       // dotted-quad source address
};

// Failure modes of parseDiscoveryResponse. kTooShort/kNameTooLong and the
// strict hex-segment device-type parse are C++ hardening: upstream either
// raises a different exception or accepts looser input there.
enum class DiscoveryError : uint8_t {
  kNone = 0,
  kTooShort,      // cannot even hold the device-id field (or bad version)
  kDecryptFailed, // encrypted region missing/oversized/not block-aligned or
                  // AES-ECB/PKCS7 failure
  kBadName,       // name lacks a "_<hex>" type segment
  kNameTooLong,   // name longer than kDiscoveryNameCapacity - 1
};

// Parses one discovery datagram body. version must come from
// getDiscoveryResponse; sourceIp (the UDP sender) is copied verbatim into
// out->ip. Trailing bytes after the name (present on V3) are ignored.
DiscoveryError parseDiscoveryResponse(const uint8_t* data, size_t len, DiscoveryVersion version, const char* sourceIp,
                                      DiscoveryDeviceInfo* out);

} // namespace midea
