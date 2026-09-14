#include "Settings.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstring>

namespace settings {

namespace {

// True when `v` is absent (caller keeps the current value) or a string that
// fits `dest` including the NUL.
bool readFixedString(JsonVariantConst v, char* dest, size_t destSize) {
  if (v.isNull()) return true;
  if (!v.is<const char*>()) return false;
  const char* s = v.as<const char*>();
  const size_t len = std::strlen(s);
  if (len + 1 > destSize) return false;
  std::memcpy(dest, s, len + 1);
  return true;
}

// Optional JSON coordinate: a number within ±`maxAbsE4` (in 1e-4 degrees),
// stored scaled. ArduinoJson hands back a float32 for fractional JSON
// numbers, whose ~7-digit precision covers the 1e-4 grid but not
// microdegrees — the llround absorbs the representation wobble. Absent
// keeps the current value; anything else rejects the document (same
// all-or-nothing contract as the string fields).
bool readCoordE4(JsonVariantConst v, int32_t maxAbsE4, int32_t& out) {
  if (v.isNull()) return true;
  if (!v.is<double>()) return false;
  const double scaled = v.as<double>() * 10000.0;
  if (!(scaled >= -static_cast<double>(maxAbsE4) && scaled <= static_cast<double>(maxAbsE4))) return false;
  out = static_cast<int32_t>(llround(scaled));
  return true;
}

uint32_t readU32Le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

void writeU32Le(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint16_t readU16Le(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

void writeU16Le(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

// Legacy (v1/v2) binary offsets: the single credentials pair and, for v2,
// the weather block.
constexpr size_t kBinLegacySsid = 2;
constexpr size_t kBinLegacyPassword = kBinLegacySsid + kSsidMaxLen + 1; // 35
constexpr size_t kBinWeatherEnabled = 100;
constexpr size_t kBinWeatherName = 101;
constexpr size_t kBinWeatherLat = 118;
constexpr size_t kBinWeatherLon = 122;

// v3 binary layout: the active-profile byte, then fixed-size network slots,
// then the weather block.
constexpr size_t kBinActive = 2;
constexpr size_t kBinNetworks = 4;
constexpr size_t kNetworkBinSize = kSsidMaxLen + 1 + kPasswordMaxLen + 1 + 1; // 99
constexpr size_t kNetworkPortalOffset = kSsidMaxLen + 1 + kPasswordMaxLen;    // 98

// v4: the WireGuard block starts after the weather block (400 + 32).
constexpr size_t kBinWg = 432;
constexpr size_t kBinWgEndpoint = kBinWg + 1;      // 433, 65 B
constexpr size_t kBinWgPort = kBinWgEndpoint + 65; // 498
constexpr size_t kBinWgPriv = kBinWgPort + 2;      // 500, 45 B
constexpr size_t kBinWgPub = kBinWgPriv + 45;      // 545, 45 B
constexpr size_t kBinWgIp = kBinWgPub + 45;        // 590, 16 B
constexpr size_t kBinWgKeepalive = kBinWgIp + 16;  // 606
static_assert(kBinWgKeepalive + 2 == kBinPayloadSizeV4, "v4 binary layout must fill 608 B");

// v5: the AC block starts after the WireGuard block.
constexpr size_t kBinAc = kBinPayloadSizeV4; // 608
constexpr size_t kBinAcButtonLock = kBinAc + 1;
static_assert(kBinAcButtonLock + 1 == kBinPayloadSize, "v5 binary layout must fill 610 B");

// Strict copy of one binary network slot's string fields: a field without a
// NUL inside its window means corruption.
bool loadNetworkFields(const uint8_t* src, Network& net) {
  std::memcpy(net.ssid, src, sizeof(net.ssid));
  std::memcpy(net.password, src + sizeof(net.ssid), sizeof(net.password));
  return std::memchr(net.ssid, 0, sizeof(net.ssid)) != nullptr &&
         std::memchr(net.password, 0, sizeof(net.password)) != nullptr;
}

} // namespace

const Network* activeNetwork(const Settings& s) {
  if (s.activeNetwork >= kMaxNetworks) return nullptr;
  const Network& net = s.networks[s.activeNetwork];
  return net.ssid[0] != '\0' ? &net : nullptr;
}

bool anyNetwork(const Settings& s) {
  for (const Network& net : s.networks) {
    if (net.ssid[0] != '\0') return true;
  }
  return false;
}

bool weatherConfigured(const Weather& w) {
  return w.latE4 != 0 && w.lonE4 != 0 && w.latE4 >= -kLatMaxE4 && w.latE4 <= kLatMaxE4 && w.lonE4 >= -kLonMaxE4 &&
         w.lonE4 <= kLonMaxE4;
}

bool wireGuardConfigured(const WireGuard& w) {
  return w.endpoint[0] != '\0' && w.port != 0 && std::strlen(w.ownPrivateKey) == kWgKeyLen &&
         std::strlen(w.peerPublicKey) == kWgKeyLen && w.ownIp[0] != '\0';
}

size_t serialize(const Settings& s, char* buf, size_t cap) {
  if (!buf || cap == 0) return 0;

  JsonDocument doc;
  doc["v"] = s.schemaVersion;
  // All slots are emitted (empty ones too) so JSON keeps the slot positions
  // the "active" index refers to.
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (const Network& net : s.networks) {
    JsonObject entry = networks.add<JsonObject>();
    entry["ssid"] = net.ssid;
    entry["pass"] = net.password;
    entry["portal"] = net.portalState;
  }
  // kNoNetwork is the absent state: omitting the key reads back as kNoNetwork.
  if (s.activeNetwork != kNoNetwork) {
    doc["active"] = s.activeNetwork;
  }
  JsonObject weather = doc["weather"].to<JsonObject>();
  weather["en"] = s.weather.enabled ? 1 : 0;
  weather["name"] = s.weather.name;
  // 1e-4 degrees scaled back to degrees; ArduinoJson emits the shortest
  // round-trip form, so 52.52 stays "52.52".
  weather["lat"] = static_cast<double>(s.weather.latE4) / 10000.0;
  weather["lon"] = static_cast<double>(s.weather.lonE4) / 10000.0;
  JsonObject wg = doc["wg"].to<JsonObject>();
  wg["en"] = s.wireguard.enabled ? 1 : 0;
  wg["ep"] = s.wireguard.endpoint;
  wg["port"] = s.wireguard.port;
  wg["priv"] = s.wireguard.ownPrivateKey;
  wg["pub"] = s.wireguard.peerPublicKey;
  wg["ip"] = s.wireguard.ownIp;
  wg["ka"] = s.wireguard.keepalive;
  JsonObject ac = doc["ac"].to<JsonObject>();
  ac["beep"] = s.ac.beep ? 1 : 0;
  ac["lock"] = s.ac.buttonLock ? 1 : 0;

  // measureJson first: serializeJson truncates silently and reports the
  // truncated length, so the exact need has to be checked up front.
  const size_t need = measureJson(doc);
  if (need + 1 > cap) return 0;
  return serializeJson(doc, buf, cap);
}

bool deserialize(const char* json, Settings& out) {
  if (!json) return false;

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return false;
  if (!doc.is<JsonObjectConst>()) return false;

  const JsonVariantConst version = doc["v"];
  if (!version.is<uint32_t>() || version.as<uint32_t>() < kMinSupportedSchemaVersion ||
      version.as<uint32_t>() > kSchemaVersion) {
    return false;
  }
  const uint32_t docVersion = version.as<uint32_t>();

  Settings next = out;

  if (docVersion < 3) {
    // v1/v2 documents carry a single pair: migrate it into profile 0 and
    // claim it active when nothing else is.
    if (!readFixedString(doc["ssid"], next.networks[0].ssid, sizeof(next.networks[0].ssid))) return false;
    if (!readFixedString(doc["pass"], next.networks[0].password, sizeof(next.networks[0].password))) return false;
    if (next.networks[0].ssid[0] != '\0' && next.activeNetwork == kNoNetwork) {
      next.activeNetwork = 0;
    }
  } else {
    const JsonVariantConst networks = doc["networks"];
    if (!networks.isNull()) {
      if (!networks.is<JsonArrayConst>()) return false;
      const JsonArrayConst array = networks.as<JsonArrayConst>();
      if (array.size() > kMaxNetworks) return false;
      // A present array is a full replacement of the profile list.
      Network fresh[kMaxNetworks];
      uint8_t slot = 0;
      for (JsonVariantConst entry : array) {
        if (!entry.is<JsonObjectConst>()) return false;
        const JsonObjectConst obj = entry.as<JsonObjectConst>();
        if (!readFixedString(obj["ssid"], fresh[slot].ssid, sizeof(fresh[slot].ssid))) return false;
        if (!readFixedString(obj["pass"], fresh[slot].password, sizeof(fresh[slot].password))) return false;
        const JsonVariantConst portal = obj["portal"];
        if (!portal.isNull()) {
          if (!portal.is<uint32_t>() || portal.as<uint32_t>() > kPortalFound) return false;
          fresh[slot].portalState = static_cast<uint8_t>(portal.as<uint32_t>());
        }
        slot++;
      }
      std::memcpy(next.networks, fresh, sizeof(next.networks));
    }
    const JsonVariantConst active = doc["active"];
    if (!active.isNull()) {
      if (!active.is<uint32_t>() || (active.as<uint32_t>() != kNoNetwork && active.as<uint32_t>() >= kMaxNetworks)) {
        return false;
      }
      next.activeNetwork = static_cast<uint8_t>(active.as<uint32_t>());
      // An explicit active must name a filled slot.
      if (next.activeNetwork != kNoNetwork && next.networks[next.activeNetwork].ssid[0] == '\0') return false;
    } else if (next.activeNetwork != kNoNetwork && next.networks[next.activeNetwork].ssid[0] == '\0') {
      // Absent active inherited from the caller but a replaced list invalidated
      // it: fall back to "nothing active" rather than rejecting.
      next.activeNetwork = kNoNetwork;
    }
  }

  // Weather block: absent (v1 documents, or hand-written JSON) keeps the
  // defaults already in `next`; a present block must be well-formed.
  const JsonVariantConst weather = doc["weather"];
  if (!weather.isNull()) {
    if (!weather.is<JsonObjectConst>()) return false;
    const JsonObjectConst w = weather.as<JsonObjectConst>();
    const JsonVariantConst en = w["en"];
    if (!en.isNull()) {
      if (en.is<bool>()) {
        next.weather.enabled = en.as<bool>();
      } else if (en.is<int>() && (en.as<int>() == 0 || en.as<int>() == 1)) {
        next.weather.enabled = en.as<int>() != 0;
      } else {
        return false;
      }
    }
    if (!readFixedString(w["name"], next.weather.name, sizeof(next.weather.name))) return false;
    if (!readCoordE4(w["lat"], kLatMaxE4, next.weather.latE4)) return false;
    if (!readCoordE4(w["lon"], kLonMaxE4, next.weather.lonE4)) return false;
  }

  // WireGuard block (v4): absent keeps the current values (v1–v3 documents);
  // a present block must be well-formed. Shape-only checks here — whether
  // the fields add up to a usable tunnel is wireGuardConfigured().
  const JsonVariantConst wg = doc["wg"];
  if (!wg.isNull()) {
    if (!wg.is<JsonObjectConst>()) return false;
    const JsonObjectConst g = wg.as<JsonObjectConst>();
    const JsonVariantConst en = g["en"];
    if (!en.isNull()) {
      if (en.is<bool>()) {
        next.wireguard.enabled = en.as<bool>();
      } else if (en.is<int>() && (en.as<int>() == 0 || en.as<int>() == 1)) {
        next.wireguard.enabled = en.as<int>() != 0;
      } else {
        return false;
      }
    }
    if (!readFixedString(g["ep"], next.wireguard.endpoint, sizeof(next.wireguard.endpoint))) return false;
    const JsonVariantConst port = g["port"];
    if (!port.isNull()) {
      if (!port.is<uint32_t>() || port.as<uint32_t>() > 65535) return false;
      next.wireguard.port = static_cast<uint16_t>(port.as<uint32_t>());
    }
    if (!readFixedString(g["priv"], next.wireguard.ownPrivateKey, sizeof(next.wireguard.ownPrivateKey))) return false;
    if (!readFixedString(g["pub"], next.wireguard.peerPublicKey, sizeof(next.wireguard.peerPublicKey))) return false;
    if (!readFixedString(g["ip"], next.wireguard.ownIp, sizeof(next.wireguard.ownIp))) return false;
    const JsonVariantConst ka = g["ka"];
    if (!ka.isNull()) {
      if (!ka.is<uint32_t>() || ka.as<uint32_t>() > kWgKeepaliveMaxSecs) return false;
      next.wireguard.keepalive = static_cast<uint16_t>(ka.as<uint32_t>());
    }
  }

  // AC block (v5): absent keeps the defaults; a present block must be
  // well-formed. bools accept true/false and 0/1 like the other en fields.
  const JsonVariantConst ac = doc["ac"];
  if (!ac.isNull()) {
    if (!ac.is<JsonObjectConst>()) return false;
    const JsonObjectConst a = ac.as<JsonObjectConst>();
    const JsonVariantConst beep = a["beep"];
    if (!beep.isNull()) {
      if (beep.is<bool>()) {
        next.ac.beep = beep.as<bool>();
      } else if (beep.is<int>() && (beep.as<int>() == 0 || beep.as<int>() == 1)) {
        next.ac.beep = beep.as<int>() != 0;
      } else {
        return false;
      }
    }
    const JsonVariantConst lock = a["lock"];
    if (!lock.isNull()) {
      if (lock.is<bool>()) {
        next.ac.buttonLock = lock.as<bool>();
      } else if (lock.is<int>() && (lock.as<int>() == 0 || lock.as<int>() == 1)) {
        next.ac.buttonLock = lock.as<int>() != 0;
      } else {
        return false;
      }
    }
  }
  next.schemaVersion = kSchemaVersion;

  out = next;
  return true;
}

void serializeBin(const Settings& s, uint8_t* out) {
  std::memset(out, 0, kBinPayloadSize);
  out[0] = static_cast<uint8_t>(kSchemaVersion & 0xFF);
  out[1] = static_cast<uint8_t>(kSchemaVersion >> 8);
  out[kBinActive] = s.activeNetwork;
  for (size_t i = 0; i < kMaxNetworks; i++) {
    uint8_t* slot = out + kBinNetworks + i * kNetworkBinSize;
    std::memcpy(slot, s.networks[i].ssid, sizeof(s.networks[i].ssid));
    std::memcpy(slot + sizeof(s.networks[i].ssid), s.networks[i].password, sizeof(s.networks[i].password));
    slot[kNetworkPortalOffset] = s.networks[i].portalState;
  }
  const size_t weatherBase = kBinNetworks + kMaxNetworks * kNetworkBinSize; // 400
  out[weatherBase] = s.weather.enabled ? 1 : 0;
  std::memcpy(out + weatherBase + 1, s.weather.name, sizeof(s.weather.name));
  writeU32Le(out + weatherBase + 18, static_cast<uint32_t>(s.weather.latE4));
  writeU32Le(out + weatherBase + 22, static_cast<uint32_t>(s.weather.lonE4));
  out[kBinWg] = s.wireguard.enabled ? 1 : 0;
  std::memcpy(out + kBinWgEndpoint, s.wireguard.endpoint, kWgEndpointMaxLen + 1);
  writeU16Le(out + kBinWgPort, s.wireguard.port);
  std::memcpy(out + kBinWgPriv, s.wireguard.ownPrivateKey, kWgKeyLen + 1);
  std::memcpy(out + kBinWgPub, s.wireguard.peerPublicKey, kWgKeyLen + 1);
  std::memcpy(out + kBinWgIp, s.wireguard.ownIp, kWgIpMaxLen + 1);
  writeU16Le(out + kBinWgKeepalive, s.wireguard.keepalive);
  out[kBinAc] = s.ac.beep ? 1 : 0;
  out[kBinAcButtonLock] = s.ac.buttonLock ? 1 : 0;
}

namespace {

// Shared weather-block parse for the v2 and v3 binary layouts. `base` is the
// offset of the enabled byte.
bool parseWeatherBin(const uint8_t* in, size_t base, Settings& next) {
  if (in[base] > 1) return false;
  next.weather.enabled = in[base] != 0;
  std::memcpy(next.weather.name, in + base + 1, sizeof(next.weather.name));
  if (!std::memchr(next.weather.name, 0, sizeof(next.weather.name))) return false;
  next.weather.latE4 = static_cast<int32_t>(readU32Le(in + base + 18));
  next.weather.lonE4 = static_cast<int32_t>(readU32Le(in + base + 22));
  if (next.weather.latE4 < -kLatMaxE4 || next.weather.latE4 > kLatMaxE4 || next.weather.lonE4 < -kLonMaxE4 ||
      next.weather.lonE4 > kLonMaxE4) {
    return false;
  }
  return true;
}

// Shared v3 core parse (networks + weather) for the v3 and v4 payloads.
bool parseV3Core(const uint8_t* in, Settings& next) {
  next.activeNetwork = in[kBinActive];
  if (in[kBinActive + 1] != 0) return false; // reserved byte
  for (size_t i = 0; i < kMaxNetworks; i++) {
    const uint8_t* slot = in + kBinNetworks + i * kNetworkBinSize;
    if (!loadNetworkFields(slot, next.networks[i])) return false;
    if (slot[kNetworkPortalOffset] > kPortalFound) return false;
    next.networks[i].portalState = slot[kNetworkPortalOffset];
  }
  if (next.activeNetwork != kNoNetwork) {
    if (next.activeNetwork >= kMaxNetworks || next.networks[next.activeNetwork].ssid[0] == '\0') return false;
  }
  return parseWeatherBin(in, kBinNetworks + kMaxNetworks * kNetworkBinSize, next);
}

// v4 WireGuard block. Strings must be NUL-terminated inside their window;
// keepalive stays in range. Empty-but-valid values are allowed — whether
// the block adds up to a tunnel is wireGuardConfigured().
bool parseWireGuardBin(const uint8_t* in, Settings& next) {
  if (in[kBinWg] > 1) return false;
  next.wireguard.enabled = in[kBinWg] != 0;
  std::memcpy(next.wireguard.endpoint, in + kBinWgEndpoint, sizeof(next.wireguard.endpoint));
  std::memcpy(next.wireguard.ownPrivateKey, in + kBinWgPriv, sizeof(next.wireguard.ownPrivateKey));
  std::memcpy(next.wireguard.peerPublicKey, in + kBinWgPub, sizeof(next.wireguard.peerPublicKey));
  std::memcpy(next.wireguard.ownIp, in + kBinWgIp, sizeof(next.wireguard.ownIp));
  if (!std::memchr(next.wireguard.endpoint, 0, sizeof(next.wireguard.endpoint)) ||
      !std::memchr(next.wireguard.ownPrivateKey, 0, sizeof(next.wireguard.ownPrivateKey)) ||
      !std::memchr(next.wireguard.peerPublicKey, 0, sizeof(next.wireguard.peerPublicKey)) ||
      !std::memchr(next.wireguard.ownIp, 0, sizeof(next.wireguard.ownIp))) {
    return false;
  }
  next.wireguard.port = readU16Le(in + kBinWgPort);
  next.wireguard.keepalive = readU16Le(in + kBinWgKeepalive);
  if (next.wireguard.keepalive > kWgKeepaliveMaxSecs) return false;
  return true;
}

// v5 AC block: two strict 0/1 bytes.
bool parseAcBin(const uint8_t* in, Settings& next) {
  if (in[kBinAc] > 1 || in[kBinAcButtonLock] > 1) return false;
  next.ac.beep = in[kBinAc] != 0;
  next.ac.buttonLock = in[kBinAcButtonLock] != 0;
  return true;
}

} // namespace

bool deserializeBin(const uint8_t* in, size_t len, Settings& out) {
  if (!in) return false;
  const uint16_t version = static_cast<uint16_t>(in[0] | (in[1] << 8));

  Settings next;

  if (len == kBinPayloadSize) {
    // Version and length are bound 1:1; the v5 payload is 610 B.
    if (version != 5) return false;
    if (!parseV3Core(in, next)) return false;
    if (!parseWireGuardBin(in, next)) return false;
    if (!parseAcBin(in, next)) return false;
  } else if (len == kBinPayloadSizeV4) {
    // v4 (608 B): no AC block; it stays at its defaults.
    if (version != 4) return false;
    if (!parseV3Core(in, next)) return false;
    if (!parseWireGuardBin(in, next)) return false;
  } else if (len == kBinPayloadSizeV3) {
    // v3 (432 B): no WireGuard block; it stays at its defaults.
    if (version != 3) return false;
    if (!parseV3Core(in, next)) return false;
  } else if (len == kBinPayloadSizeV2) {
    if (version != 2) return false;
    if (!loadNetworkFields(in + kBinLegacySsid, next.networks[0])) return false;
    if (next.networks[0].ssid[0] != '\0') next.activeNetwork = 0;
    if (!parseWeatherBin(in, kBinWeatherEnabled, next)) return false;
  } else if (len == kBinPayloadSizeV1) {
    if (version != 1) return false;
    if (!loadNetworkFields(in + kBinLegacySsid, next.networks[0])) return false;
    if (next.networks[0].ssid[0] != '\0') next.activeNetwork = 0;
    // v1 payloads carry no weather block: `next.weather` keeps its defaults.
  } else {
    return false;
  }

  next.schemaVersion = kSchemaVersion;

  out = next;
  return true;
}

} // namespace settings
