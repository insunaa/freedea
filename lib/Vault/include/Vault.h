#pragma once

// Encrypted-at-rest envelope for SD config files (3.4). Threat model: a
// lost/stolen SD card — the key derives from the chip's EFuse Wi-Fi MAC and
// never touches the card, so the card alone cannot decrypt. It is *not* a
// defense against someone holding the device (open firmware, JTAG-readable
// MAC); the keySrc header byte reserves a later EFuse-HMAC upgrade.
//
// Envelope (little-endian header, 32 bytes):
//   0  magic 'F','D','V','1'
//   4  kind        (Kind)
//   5  format      (= kFormatVersion)
//   6  key source  (= kKeySrcMac; reserved: 2 = EFuse HMAC)
//   7  reserved    (0)
//   8  payloadLen  u16 (0..65535, real length inside the padded ciphertext)
//   10 reserved    u16 (0)
//   12 crc32       u32 over bytes [16, end) — iv||ciphertext
//   16 iv[16]      random per write
//   32 ciphertext  AES-256-CBC over the payload zero-padded to 16
//
// The CRC is verified before decrypting, so corruption is reported even with
// the wrong key. A wrong key (SD card from another device) passes CRC and
// surfaces as a payload version mismatch at the caller — callers must version
// their payload struct and treat mismatch as "foreign card", never trigger
// slot repair on it.
//
// Pure C++ (AES shim mirrors lib/MideaAC Security: esp_aes_* on device,
// software mbedtls on host) so test/host covers it without hardware.

#include <cstddef>
#include <cstdint>

namespace vault {

inline constexpr size_t kKeyLen = 32;
inline constexpr size_t kIvLen = 16;
inline constexpr size_t kMacLen = 6;
inline constexpr size_t kHeaderLen = 32;
inline constexpr uint8_t kFormatVersion = 1;
inline constexpr uint8_t kKeySrcMac = 1;

// Payload families riding the envelope. Future stretch-goal JSON settings
// (weather, schedules) get new values; the envelope itself is payload-agnostic.
enum class Kind : uint8_t {
  kSettings = 1,
  kDevices = 2,
};

// Store key = sha256("freedea:vault:v1" || mac). Pure function; firmware
// feeds the esp_read_mac(ESP_MAC_WIFI_STA) bytes.
void deriveKey(const uint8_t mac[kMacLen], uint8_t keyOut[kKeyLen]);

// CRC-32/ISO-HDLC (reflected 0xEDB88320, init/final complemented — matches
// zlib). Bitwise: no 1 KB table, files here are small and touched rarely.
uint32_t crc32(const uint8_t* data, size_t len);

// Ciphertext size for a payload and total sealed size.
constexpr size_t paddedSize(size_t payloadLen) {
  return (payloadLen + 15) & ~static_cast<size_t>(15);
}
constexpr size_t sealSize(size_t payloadLen) {
  return kHeaderLen + paddedSize(payloadLen);
}

// Encrypts and seals a payload into out; returns the byte count written or 0
// on invalid arguments/capacity. iv must be fresh random per write.
size_t seal(Kind kind, const uint8_t key[kKeyLen], const uint8_t iv[kIvLen], const uint8_t* plain, size_t plainLen,
            uint8_t* out, size_t outCap);

enum class OpenError : uint8_t {
  kNone = 0,
  kTooShort,  // smaller than the header
  kBadMagic,  // not our file
  kBadFormat, // unknown envelope version
  kBadKind,   // file is for a different payload family
  kCorrupt,   // CRC mismatch, non-block ciphertext, or oversized payloadLen
  kInternal,  // AES failure
};

// Verifies the envelope, then decrypts into plainOut; *plainLenOut gets the
// stored payload length (<= outCap enforced). The decrypted bytes are NOT
// trusted: callers must validate their payload's own version/magic (that is
// also how a wrong key is detected — it decrypts to garbage that passes CRC).
OpenError open(Kind kind, const uint8_t key[kKeyLen], const uint8_t* in, size_t inLen, uint8_t* plainOut, size_t outCap,
               size_t* plainLenOut);

} // namespace vault
