#include "Discovery.h"

#include <cstdio>
#include <cstring>

#include "Security.h"

namespace midea {
namespace {

// int.from_bytes(view[20:26], "little"): 6-byte little-endian device id.
// Byte-wise to stay clear of the RISC-V unaligned-load hazard.
uint64_t readLe6(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 5; i >= 0; --i) {
    value = (value << 8) | p[i];
  }
  return value;
}

// int(segment, 16) restricted to 1-4 plain hex digits (observed segments are
// two, e.g. "ac" in "net_ac_F7B4").
bool parseHexType(const char* s, size_t len, uint16_t* out) {
  if (len == 0 || len > 4) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < len; ++i) {
    const char c = s[i];
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<uint32_t>(c - 'A' + 10);
    } else {
      return false;
    }
    value = value * 16 + digit;
  }
  *out = static_cast<uint16_t>(value);
  return true;
}

} // namespace

DiscoveryVersion getDiscoveryVersion(const uint8_t* data, size_t len) {
  if (data != nullptr && len >= 2) {
    if (data[0] == 0x5A && data[1] == 0x5A) return DiscoveryVersion::kV2;
    if (data[0] == 0x83 && data[1] == 0x70) return DiscoveryVersion::kV3;
  }
  return DiscoveryVersion::kUnsupported;
}

DiscoveryError parseDiscoveryResponse(const uint8_t* data, size_t len, DiscoveryVersion version, const char* sourceIp,
                                      DiscoveryDeviceInfo* out) {
  if (data == nullptr || sourceIp == nullptr || out == nullptr) return DiscoveryError::kTooShort;

  // V3 carries an 8-byte wrapper header and a 16-byte trailing hash that the
  // common parser first strips (discover.py:298-302); V2 uses the whole
  // datagram.
  const size_t strip = version == DiscoveryVersion::kV3 ? 8 : 0;
  const size_t tailTrim = version == DiscoveryVersion::kV3 ? 16 : 0;
  if (version != DiscoveryVersion::kV2 && version != DiscoveryVersion::kV3) return DiscoveryError::kTooShort;
  if (len < strip + tailTrim) return DiscoveryError::kTooShort;
  const uint8_t* view = data + strip;
  const size_t viewLen = len - strip - tailTrim;
  if (viewLen < 26) return DiscoveryError::kTooShort; // device id field

  out->deviceId = readLe6(view + 20);

  // Encrypted payload = view[40:-16] (discover.py:301).
  if (viewLen < 56) return DiscoveryError::kDecryptFailed;
  const size_t encLen = viewLen - 56;
  if (encLen > kDiscoveryMaxDecrypted || encLen % kAesBlockSize != 0) return DiscoveryError::kDecryptFailed;

  uint8_t dec[kDiscoveryMaxDecrypted];
  size_t decLen = 0;
  if (!decryptAesPkcs7(view + 40, encLen, dec, sizeof(dec), &decLen)) return DiscoveryError::kDecryptFailed;
  if (decLen < 41) return DiscoveryError::kTooShort;

  // dec[3::-1] is the reported IPv4 address; it is only ever used by upstream
  // for a mismatch warning, so the source address is reported instead.
  out->port = static_cast<uint16_t>(dec[4] | (static_cast<uint16_t>(dec[5]) << 8));
  std::memcpy(out->sn, dec + 8, 32);
  out->sn[32] = '\0';

  const uint8_t nameLen = dec[40];
  if (nameLen >= kDiscoveryNameCapacity) return DiscoveryError::kNameTooLong;
  if (decLen < 41u + nameLen) return DiscoveryError::kTooShort;
  std::memcpy(out->name, dec + 41, nameLen);
  out->name[nameLen] = '\0';

  // device_type = int(name.split("_")[1], 16): the segment between the first
  // and second underscore.
  const char* first = static_cast<const char*>(std::memchr(out->name, '_', nameLen));
  if (first == nullptr) return DiscoveryError::kBadName;
  const char* segment = first + 1;
  const size_t rest = nameLen - static_cast<size_t>(segment - out->name);
  const char* second = static_cast<const char*>(std::memchr(segment, '_', rest));
  const size_t segmentLen = second != nullptr ? static_cast<size_t>(second - segment) : rest;
  if (!parseHexType(segment, segmentLen, &out->deviceType)) return DiscoveryError::kBadName;

  std::snprintf(out->ip, sizeof(out->ip), "%s", sourceIp);
  out->version = version;
  return DiscoveryError::kNone;
}

} // namespace midea
