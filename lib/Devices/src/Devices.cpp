#include "Devices.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace devices {

namespace {

// Record wire offsets (little-endian, always byte-copied — never cast;
// unaligned wider reads fault on the ESP32-C3).
constexpr size_t kOffId = 0;        // u64
constexpr size_t kOffVersion = 8;   // u8
constexpr size_t kOffPort = 10;     // u16
constexpr size_t kOffIp = 12;       // [4]
constexpr size_t kOffName = 16;     // [24]
constexpr size_t kOffTokenLen = 40; // u16
constexpr size_t kOffKey = 42;      // [32]
constexpr size_t kOffToken = 74;    // [128]
static_assert(kOffToken + kTokenMaxLen <= kRecordSize);
static_assert(4 + kMaxDevices * kRecordSize == kBinPayloadSize);

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU64(uint8_t* p, uint64_t v) {
  for (size_t i = 0; i < 8; ++i)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint16_t getU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

bool hexNibble(char c, uint8_t& out) {
  if (c >= '0' && c <= '9') {
    out = static_cast<uint8_t>(c - '0');
  } else if (c >= 'a' && c <= 'f') {
    out = static_cast<uint8_t>(c - 'a' + 10);
  } else if (c >= 'A' && c <= 'F') {
    out = static_cast<uint8_t>(c - 'A' + 10);
  } else {
    return false;
  }
  return true;
}

// Strict hex string → bytes; the length must be even and the bytes must fit.
bool parseHex(const char* s, size_t srcLen, uint8_t* buf, size_t bufCap, size_t& outLen) {
  if (srcLen % 2) return false;
  outLen = srcLen / 2;
  if (outLen > bufCap) return false;
  for (size_t i = 0; i < outLen; ++i) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (!hexNibble(s[2 * i], hi) || !hexNibble(s[2 * i + 1], lo)) return false;
    buf[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool parseIp(const char* s, uint8_t out[4]) {
  size_t pos = 0;
  for (int part = 0; part < 4; ++part) {
    if (part) {
      if (s[pos] != '.') return false;
      ++pos;
    }
    unsigned value = 0;
    int digits = 0;
    while (s[pos] >= '0' && s[pos] <= '9') {
      value = value * 10 + static_cast<unsigned>(s[pos] - '0');
      ++pos;
      ++digits;
    }
    if (digits == 0 || digits > 3 || value > 255) return false;
    out[part] = static_cast<uint8_t>(value);
  }
  return s[pos] == '\0';
}

bool isPrintableAscii(char c) {
  return c >= 0x20 && c <= 0x7E;
}

// id: JSON number, or decimal/hex ("0x…") string; non-zero (a real msmart id
// never is).
bool readDeviceId(JsonVariantConst v, uint64_t& id) {
  if (v.is<uint64_t>()) {
    id = v.as<uint64_t>();
    return id != 0;
  }
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(s, &end, 0);
    if (end == s || *end != '\0' || parsed == 0) return false;
    id = static_cast<uint64_t>(parsed);
    return true;
  }
  return false;
}

// Staging buffers for whole-document replace semantics; the import/parse
// paths run single-task on the main loop and never nest.
List sParsedList;
Device sParsedDevice;

} // namespace

void serializeBin(const List& list, uint8_t* out) {
  std::memset(out, 0, kBinPayloadSize);
  putU16(out, kPayloadVersion);
  putU16(out + 2, list.count);
  for (uint8_t i = 0; i < list.count && i < kMaxDevices; ++i) {
    const Device& d = list.devices[i];
    uint8_t* r = out + 4 + i * kRecordSize;
    putU64(r + kOffId, d.id);
    r[kOffVersion] = d.version;
    putU16(r + kOffPort, d.port);
    std::memcpy(r + kOffIp, d.ip, 4);
    std::memcpy(r + kOffName, d.name, kNameCapacity);
    putU16(r + kOffTokenLen, d.tokenLen);
    std::memcpy(r + kOffKey, d.key, kKeyLen);
    std::memcpy(r + kOffToken, d.token, kTokenMaxLen);
  }
}

bool deserializeBin(const uint8_t* in, size_t len, List& out) {
  if (!in || len != kBinPayloadSize) return false;
  if (getU16(in) != kPayloadVersion) return false;
  const uint16_t count = getU16(in + 2);
  if (count > kMaxDevices) return false;

  List& next = sParsedList;
  next = List{};
  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* r = in + 4 + i * kRecordSize;
    Device& d = next.devices[i];
    d.id = getU64(r + kOffId);
    if (d.id == 0) return false;
    d.version = r[kOffVersion];
    if (d.version != 0 && d.version != 2 && d.version != 3) return false;
    d.port = getU16(r + kOffPort);
    std::memcpy(d.ip, r + kOffIp, 4);
    if (!std::memchr(r + kOffName, 0, kNameCapacity)) return false;
    std::memcpy(d.name, r + kOffName, kNameCapacity);
    const uint16_t tokenLen = getU16(r + kOffTokenLen);
    if (tokenLen > kTokenMaxLen) return false;
    d.tokenLen = tokenLen;
    std::memcpy(d.key, r + kOffKey, kKeyLen);
    std::memcpy(d.token, r + kOffToken, kTokenMaxLen);
    for (uint16_t j = 0; j < i; ++j) {
      if (next.devices[j].id == d.id) return false;
    }
  }
  next.count = static_cast<uint8_t>(count);
  out = next;
  return true;
}

ImportError parseImportJson(const char* json, List& out) {
  if (!json) return ImportError::kJson;

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return ImportError::kJson;
  if (!doc.is<JsonObjectConst>()) return ImportError::kJson;

  const JsonVariantConst version = doc["v"];
  if (!version.is<uint32_t>() || version.as<uint32_t>() != kPayloadVersion) return ImportError::kVersion;

  const JsonVariantConst items = doc["devices"];
  if (!items.is<JsonArrayConst>()) return ImportError::kJson;
  const JsonArrayConst arr = items.as<JsonArrayConst>();
  if (arr.size() > kMaxDevices) return ImportError::kTooMany;

  List& next = sParsedList;
  next = List{};
  for (JsonVariantConst entry : arr) {
    if (!entry.is<JsonObjectConst>()) return ImportError::kJson;
    Device& d = sParsedDevice;
    d = Device{};

    if (!readDeviceId(entry["id"], d.id)) return ImportError::kBadId;
    for (uint8_t i = 0; i < next.count; ++i) {
      if (next.devices[i].id == d.id) return ImportError::kBadId;
    }

    const JsonVariantConst name = entry["name"];
    if (name.isNull()) {
      std::snprintf(d.name, sizeof(d.name), "AC-%04X", static_cast<unsigned>(d.id & 0xFFFFu));
    } else if (!name.is<const char*>()) {
      return ImportError::kBadName;
    } else {
      const char* n = name.as<const char*>();
      const size_t len = std::strlen(n);
      if (len == 0 || len + 1 > kNameCapacity) return ImportError::kBadName;
      for (size_t i = 0; i < len; ++i) {
        if (!isPrintableAscii(n[i])) return ImportError::kBadName;
      }
      std::memcpy(d.name, n, len + 1);
    }

    const JsonVariantConst version = entry["version"];
    if (!version.isNull()) {
      if (!version.is<uint32_t>() || (version.as<uint32_t>() != 2 && version.as<uint32_t>() != 3)) {
        return ImportError::kBadVersion;
      }
      d.version = static_cast<uint8_t>(version.as<uint32_t>());
    }

    const JsonVariantConst ip = entry["ip"];
    if (!ip.isNull() && (!ip.is<const char*>() || !parseIp(ip.as<const char*>(), d.ip))) {
      return ImportError::kBadIp;
    }

    const JsonVariantConst port = entry["port"];
    if (!port.isNull()) {
      if (!port.is<uint32_t>() || port.as<uint32_t>() < 1 || port.as<uint32_t>() > 65535) {
        return ImportError::kBadPort;
      }
      d.port = static_cast<uint16_t>(port.as<uint32_t>());
    }

    // Hex/format errors on each field are reported before pairing errors, so
    // the log points at the malformed value rather than its counterpart.
    const JsonVariantConst token = entry["token"];
    const JsonVariantConst key = entry["key"];
    const bool tokenGiven = !token.isNull();
    const bool keyGiven = !key.isNull();
    if (tokenGiven) {
      if (!token.is<const char*>()) return ImportError::kBadToken;
      const char* t = token.as<const char*>();
      size_t tokenLen = 0;
      if (!parseHex(t, std::strlen(t), d.token, kTokenMaxLen, tokenLen) || tokenLen == 0) {
        return ImportError::kBadToken;
      }
      d.tokenLen = static_cast<uint16_t>(tokenLen);
      if (!key.is<const char*>()) return ImportError::kBadKey;
      const char* k = key.as<const char*>();
      size_t keyLen = 0;
      if (!parseHex(k, std::strlen(k), d.key, kKeyLen, keyLen) || keyLen != kKeyLen) {
        return ImportError::kBadKey;
      }
    } else if (keyGiven) {
      return ImportError::kBadToken; // key without a token
    }

    next.devices[next.count++] = d;
  }

  out = next;
  return ImportError::kNone;
}

} // namespace devices
