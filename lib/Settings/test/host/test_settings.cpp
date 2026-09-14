// Host tests for the settings JSON core: round-trip, string escaping,
// rejection of corrupt/mismatched input, and the v1/v2 → v3 multi-network
// migrations (JSON and binary).
#include "Settings.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                      \
      ++failures;                                                                                                      \
    }                                                                                                                  \
  } while (0)

using settings::kPasswordMaxLen;
using settings::kSsidMaxLen;
using settings::Settings;

constexpr char kSentinel[] = "SENTINEL";

Settings sentinelSettings() {
  Settings s;
  std::strcpy(s.networks[0].ssid, kSentinel);
  std::strcpy(s.networks[0].password, kSentinel);
  s.activeNetwork = 0;
  return s;
}

char buf[settings::kSerializedMaxSize];

// --- round-trips -------------------------------------------------------------

void testDefaultRoundTrip() {
  Settings a;
  const size_t n = settings::serialize(a, buf, sizeof(buf));
  CHECK(n > 0);
  CHECK(n < sizeof(buf));
  CHECK(std::strstr(buf, "\"v\":5") != nullptr);

  Settings b = sentinelSettings();
  CHECK(settings::deserialize(buf, b));
  CHECK(b.schemaVersion == settings::kSchemaVersion);
  // Full replacement: the sentinel list is gone, nothing is active.
  CHECK(b.networks[0].ssid[0] == '\0');
  CHECK(b.activeNetwork == settings::kNoNetwork);
}

void testValuesRoundTrip() {
  Settings a;
  std::strcpy(a.networks[0].ssid, "LibreNet 2.4G");
  std::strcpy(a.networks[0].password, "p@ss\"w\\ord/#");
  a.activeNetwork = 0;
  const size_t n = settings::serialize(a, buf, sizeof(buf));
  CHECK(n > 0);

  Settings b;
  CHECK(settings::deserialize(buf, b));
  CHECK(std::strcmp(b.networks[0].ssid, "LibreNet 2.4G") == 0);
  CHECK(std::strcmp(b.networks[0].password, "p@ss\"w\\ord/#") == 0);
  CHECK(b.activeNetwork == 0);
}

void testMultiNetworkRoundTrip() {
  Settings a;
  std::strcpy(a.networks[0].ssid, "home");
  std::strcpy(a.networks[0].password, "homepassword");
  std::strcpy(a.networks[2].ssid, "Train-WiFi");
  a.networks[2].portalState = settings::kPortalFound;
  a.activeNetwork = 2;
  const size_t n = settings::serialize(a, buf, sizeof(buf));
  CHECK(n > 0);

  Settings b;
  CHECK(settings::deserialize(buf, b));
  // All slots are emitted, so JSON keeps slot positions and the active index.
  CHECK(std::strcmp(b.networks[0].ssid, "home") == 0);
  CHECK(std::strcmp(b.networks[1].ssid, "") == 0);
  CHECK(std::strcmp(b.networks[2].ssid, "Train-WiFi") == 0);
  CHECK(b.networks[2].portalState == settings::kPortalFound);
  CHECK(b.networks[0].portalState == settings::kPortalUnknown);
  CHECK(b.activeNetwork == 2);
}

void testBoundaryLengths() {
  Settings a;
  std::memset(a.networks[0].ssid, 'x', kSsidMaxLen);
  a.networks[0].ssid[kSsidMaxLen] = '\0';
  std::memset(a.networks[0].password, 'y', kPasswordMaxLen);
  a.networks[0].password[kPasswordMaxLen] = '\0';
  a.activeNetwork = 0;
  const size_t n = settings::serialize(a, buf, sizeof(buf));
  CHECK(n > 0);

  Settings b;
  CHECK(settings::deserialize(buf, b));
  CHECK(std::strlen(b.networks[0].ssid) == kSsidMaxLen);
  CHECK(std::strlen(b.networks[0].password) == kPasswordMaxLen);
  CHECK(std::strcmp(b.networks[0].password, a.networks[0].password) == 0);
}

// Worst case for kSerializedMaxSize: every slot filled with fully-escaping
// strings plus a fully-escaping weather name.
void testSerializedMaxSizeBound() {
  Settings a;
  for (uint8_t i = 0; i < settings::kMaxNetworks; i++) {
    std::memset(a.networks[i].ssid, 0x01, kSsidMaxLen); // \u0001: 6 bytes escaped
    a.networks[i].ssid[kSsidMaxLen] = '\0';
    std::memset(a.networks[i].password, 0x01, kPasswordMaxLen);
    a.networks[i].password[kPasswordMaxLen] = '\0';
  }
  std::memset(a.weather.name, 0x01, settings::kWeatherNameMaxLen);
  a.weather.name[settings::kWeatherNameMaxLen] = '\0';
  std::memset(a.wireguard.endpoint, 0x01, settings::kWgEndpointMaxLen);
  a.wireguard.endpoint[settings::kWgEndpointMaxLen] = '\0';
  std::memset(a.wireguard.ownPrivateKey, 0x01, settings::kWgKeyLen);
  a.wireguard.ownPrivateKey[settings::kWgKeyLen] = '\0';
  std::memset(a.wireguard.peerPublicKey, 0x01, settings::kWgKeyLen);
  a.wireguard.peerPublicKey[settings::kWgKeyLen] = '\0';
  std::memset(a.wireguard.ownIp, 0x01, settings::kWgIpMaxLen);
  a.wireguard.ownIp[settings::kWgIpMaxLen] = '\0';
  a.activeNetwork = 0;
  const size_t n = settings::serialize(a, buf, sizeof(buf));
  CHECK(n > 0);
  CHECK(n < sizeof(buf));
}

// --- parsing -----------------------------------------------------------------

void testUnicodeEscapeDecodes() {
  Settings s;
  CHECK(settings::deserialize("{\"v\":1,\"ssid\":\"caf\\u00e9\"}", s));
  // ArduinoJson decodes \uXXXX to UTF-8: é = C3 A9.
  CHECK(std::strcmp(s.networks[0].ssid, "caf\xC3\xA9") == 0);
}

void testV2JsonMigratesToProfileZero() {
  Settings s;
  CHECK(settings::deserialize("{\"v\":2,\"ssid\":\"OldHome\",\"pass\":\"oldpass\"}", s));
  CHECK(std::strcmp(s.networks[0].ssid, "OldHome") == 0);
  CHECK(std::strcmp(s.networks[0].password, "oldpass") == 0);
  CHECK(s.activeNetwork == 0);
  CHECK(s.networks[1].ssid[0] == '\0');
  CHECK(s.schemaVersion == settings::kSchemaVersion);
}

void testAbsentKeysKeepValues() {
  Settings s = sentinelSettings();
  CHECK(settings::deserialize("{\"v\":1}", s));
  CHECK(std::strcmp(s.networks[0].ssid, kSentinel) == 0);
  CHECK(std::strcmp(s.networks[0].password, kSentinel) == 0);

  // v3: absent "networks" keeps the caller's list, absent "active" its value.
  CHECK(settings::deserialize("{\"v\":3}", s));
  CHECK(std::strcmp(s.networks[0].ssid, kSentinel) == 0);
  CHECK(s.activeNetwork == 0);
}

void testRejectsGarbage() {
  Settings s = sentinelSettings();
  CHECK(!settings::deserialize("", s));
  CHECK(!settings::deserialize("{", s));
  CHECK(!settings::deserialize("{\"v\":1,,}", s));
  CHECK(!settings::deserialize("[1,2]", s));
  CHECK(!settings::deserialize("42", s));
  // Untouched on failure.
  CHECK(std::strcmp(s.networks[0].ssid, kSentinel) == 0);
  CHECK(std::strcmp(s.networks[0].password, kSentinel) == 0);
}

void testRejectsBadVersion() {
  Settings s;
  CHECK(!settings::deserialize("{\"v\":6,\"ssid\":\"x\"}", s));
  CHECK(!settings::deserialize("{\"v\":0,\"ssid\":\"x\"}", s));
  CHECK(!settings::deserialize("{\"ssid\":\"x\"}", s));
  CHECK(!settings::deserialize("{\"v\":\"1\",\"ssid\":\"x\"}", s));
  CHECK(!settings::deserialize("{\"v\":null,\"ssid\":\"x\"}", s));
}

void testRejectsBadFields() {
  Settings s;
  // Oversized (v1-style surface, migrates to profile 0 but still length-checked).
  const std::string longSsid(kSsidMaxLen + 1, 'a');
  const std::string big = "{\"v\":1,\"ssid\":\"" + longSsid + "\"}";
  CHECK(!settings::deserialize(big.c_str(), s));
  // Wrong type.
  CHECK(!settings::deserialize("{\"v\":1,\"ssid\":5}", s));
  CHECK(!settings::deserialize("{\"v\":1,\"pass\":true}", s));
  CHECK(std::strcmp(s.networks[0].ssid, "") == 0);
}

void testRejectsBadNetworkList() {
  Settings s;
  CHECK(!settings::deserialize("{\"v\":3,\"networks\":\"home\"}", s));
  CHECK(!settings::deserialize("{\"v\":3,\"networks\":[{\"ssid\":\"a\"},{\"ssid\":\"b\"},{\"ssid\":\"c\"},"
                               "{\"ssid\":\"d\"},{\"ssid\":\"e\"}]}",
                               s));
  CHECK(!settings::deserialize("{\"v\":3,\"networks\":[\"home\"]}", s));
  CHECK(!settings::deserialize("{\"v\":3,\"networks\":[{\"portal\":3}]}", s));
  CHECK(!settings::deserialize("{\"v\":3,\"networks\":[{\"portal\":true}]}", s));
  // active must be a valid index (or kNoNetwork) naming a filled slot.
  CHECK(!settings::deserialize("{\"v\":3,\"networks\":[{\"ssid\":\"a\"}],\"active\":1}", s));
  CHECK(!settings::deserialize("{\"v\":3,\"networks\":[{\"ssid\":\"a\"}],\"active\":\"x\"}", s));
  CHECK(settings::deserialize("{\"v\":3,\"networks\":[{\"ssid\":\"a\"}],\"active\":0}", s));
  CHECK(s.activeNetwork == 0);
  CHECK(std::strcmp(s.networks[0].ssid, "a") == 0);
}

// --- buffer handling -----------------------------------------------------------

void testTooSmallBuffer() {
  Settings a;
  std::strcpy(a.networks[0].ssid, "somewhatlongssid");
  char tight[8];
  tight[0] = 'x';
  CHECK(settings::serialize(a, tight, sizeof(tight)) == 0);
  CHECK(settings::serialize(a, nullptr, 0) == 0);
}

// --- binary payload (encrypted settings.bin) ---------------------------------

uint8_t binBuf[settings::kBinPayloadSize];

void testBinRoundTrip() {
  Settings a;
  std::strcpy(a.networks[0].ssid, "MyAp");
  std::strcpy(a.networks[0].password, "p@ss<>&-87654321");
  std::strcpy(a.networks[3].ssid, "Train");
  a.networks[3].portalState = settings::kPortalOpen;
  a.activeNetwork = 3;
  settings::serializeBin(a, binBuf);

  Settings b = sentinelSettings();
  CHECK(settings::deserializeBin(binBuf, sizeof(binBuf), b));
  CHECK(std::strcmp(b.networks[0].ssid, a.networks[0].ssid) == 0);
  CHECK(std::strcmp(b.networks[0].password, a.networks[0].password) == 0);
  CHECK(std::strcmp(b.networks[3].ssid, "Train") == 0);
  CHECK(b.networks[3].portalState == settings::kPortalOpen);
  CHECK(b.activeNetwork == 3);
  CHECK(b.schemaVersion == settings::kSchemaVersion);
}

void testBinFullSnapshotOverwrite() {
  // Unlike JSON (absent keys keep values), the binary snapshot clears fields.
  Settings a;
  settings::serializeBin(a, binBuf);
  Settings b = sentinelSettings();
  std::strcpy(b.networks[0].password, "a-fairly-long-stored-password-value");
  CHECK(settings::deserializeBin(binBuf, sizeof(binBuf), b));
  CHECK(b.networks[0].ssid[0] == '\0');
  CHECK(b.networks[0].password[0] == '\0');
  CHECK(b.activeNetwork == settings::kNoNetwork);
}

void testBinBoundaryLengths() {
  Settings a;
  std::memset(a.networks[0].ssid, 'S', kSsidMaxLen);
  a.networks[0].ssid[kSsidMaxLen] = '\0';
  std::memset(a.networks[0].password, 'P', kPasswordMaxLen);
  a.networks[0].password[kPasswordMaxLen] = '\0';
  a.activeNetwork = 0;
  settings::serializeBin(a, binBuf);

  Settings b;
  CHECK(settings::deserializeBin(binBuf, sizeof(binBuf), b));
  CHECK(std::strcmp(b.networks[0].ssid, a.networks[0].ssid) == 0);
  CHECK(std::strcmp(b.networks[0].password, a.networks[0].password) == 0);
}

void testBinRejectsCorrupt() {
  Settings a;
  std::strcpy(a.networks[0].ssid, "net");
  a.activeNetwork = 0;
  settings::serializeBin(a, binBuf);
  Settings b = sentinelSettings();
  const Settings good = b;

  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf) - 1, b));
  CHECK(std::strcmp(b.networks[0].ssid, good.networks[0].ssid) == 0); // untouched
  binBuf[0] ^= 0xFF;                                                  // version byte
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));

  settings::serializeBin(a, binBuf);
  constexpr size_t kNetBase = 4;
  std::memset(binBuf + kNetBase, 'X', kSsidMaxLen + 1); // no NUL in ssid window
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));

  // Portal state beyond kPortalFound is corruption.
  settings::serializeBin(a, binBuf);
  binBuf[kNetBase + kSsidMaxLen + 1 + kPasswordMaxLen] = 3;
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));

  // An active index naming an empty slot is corruption.
  settings::serializeBin(a, binBuf);
  binBuf[2] = 3;
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));

  // Reserved byte after the active index must stay zero.
  settings::serializeBin(a, binBuf);
  binBuf[3] = 1;
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));
}

// --- weather block (schema v2, unchanged path) --------------------------------

void testJsonWeatherRoundTrip() {
  Settings a;
  a.weather.enabled = true;
  std::strcpy(a.weather.name, "Berlin");
  a.weather.latE4 = 525244; // 52.5244
  a.weather.lonE4 = 134101; // 13.4101 (negative below tests the sign)
  const size_t n = settings::serialize(a, buf, sizeof(buf));
  CHECK(n > 0);

  Settings b;
  CHECK(settings::deserialize(buf, b));
  CHECK(b.weather.enabled);
  CHECK(std::strcmp(b.weather.name, "Berlin") == 0);
  CHECK(b.weather.latE4 == 525244);
  CHECK(b.weather.lonE4 == 134101);
  CHECK(settings::weatherConfigured(b.weather));

  a.weather.latE4 = -338680; // -33.868
  settings::serialize(a, buf, sizeof(buf));
  Settings c;
  CHECK(settings::deserialize(buf, c));
  CHECK(c.weather.latE4 == -338680);
}

void testJsonWeatherDefaultsAndKeep() {
  // v1 document (no weather block): loads with weather defaults.
  Settings s;
  CHECK(settings::deserialize("{\"v\":1,\"ssid\":\"x\"}", s));
  CHECK(!s.weather.enabled);
  CHECK(s.weather.name[0] == '\0');
  CHECK(s.weather.latE4 == 0 && s.weather.lonE4 == 0);
  CHECK(!settings::weatherConfigured(s.weather));

  // v2 document without the block keeps the caller's weather values.
  Settings k = sentinelSettings();
  k.weather.enabled = true;
  k.weather.latE4 = 1;
  CHECK(settings::deserialize("{\"v\":2,\"ssid\":\"y\"}", k));
  CHECK(k.weather.enabled);
  CHECK(k.weather.latE4 == 1);
}

void testJsonWeatherRejects() {
  Settings s;
  CHECK(!settings::deserialize("{\"v\":2,\"weather\":\"x\"}", s));
  CHECK(!settings::deserialize("{\"v\":2,\"weather\":{\"lat\":\"x\"}}", s));
  CHECK(!settings::deserialize("{\"v\":2,\"weather\":{\"lat\":91.0}}", s));
  CHECK(!settings::deserialize("{\"v\":2,\"weather\":{\"lon\":-180.5}}", s));
  CHECK(!settings::deserialize("{\"v\":2,\"weather\":{\"en\":7}}", s));
  CHECK(!settings::deserialize("{\"v\":2,\"weather\":{\"en\":-1}}", s));
  const std::string bigName(settings::kWeatherNameMaxLen + 1, 'n');
  const std::string big = "{\"v\":2,\"weather\":{\"name\":\"" + bigName + "\"}}";
  CHECK(!settings::deserialize(big.c_str(), s));
  CHECK(std::strcmp(s.networks[0].ssid, "") == 0);
}

// --- legacy binary migrations ---------------------------------------------------

// Hand-built v1 binary payload (112 B: version, ssid, password, reserved).
void testBinV1Migrates() {
  uint8_t v1[settings::kBinPayloadSizeV1] = {};
  v1[0] = 1;
  std::memcpy(v1 + 2, "OldAp", 6);
  std::memcpy(v1 + 2 + settings::kSsidMaxLen + 1, "oldpassword", 12); // ssid window is 33 B

  Settings b = sentinelSettings();
  CHECK(settings::deserializeBin(v1, sizeof(v1), b));
  CHECK(std::strcmp(b.networks[0].ssid, "OldAp") == 0);
  CHECK(std::strcmp(b.networks[0].password, "oldpassword") == 0);
  CHECK(b.activeNetwork == 0);
  CHECK(!b.weather.enabled);
  CHECK(b.weather.latE4 == 0 && b.weather.lonE4 == 0);
  // Tagged as current so the next save upgrades the store.
  CHECK(b.schemaVersion == settings::kSchemaVersion);
}

// Hand-built v2 binary payload (144 B: v1 prefix plus the weather block).
void testBinV2Migrates() {
  uint8_t v2[settings::kBinPayloadSizeV2] = {};
  v2[0] = 2;
  std::memcpy(v2 + 2, "Home24", 7);
  std::memcpy(v2 + 35, "wpa2passphrase", 15);
  v2[100] = 1; // weather enabled
  std::memcpy(v2 + 101, "Berlin", 7);
  v2[118] = 0xBC; // 525244 = 0x000803BC LE
  v2[119] = 0x03;
  v2[120] = 0x08;
  v2[121] = 0x00;

  Settings b = sentinelSettings();
  CHECK(settings::deserializeBin(v2, sizeof(v2), b));
  CHECK(std::strcmp(b.networks[0].ssid, "Home24") == 0);
  CHECK(std::strcmp(b.networks[0].password, "wpa2passphrase") == 0);
  CHECK(b.activeNetwork == 0);
  CHECK(b.weather.enabled);
  CHECK(std::strcmp(b.weather.name, "Berlin") == 0);
  CHECK(b.weather.latE4 == 525244);
  CHECK(b.schemaVersion == settings::kSchemaVersion);
}

void testBinV2RoundTripThroughV3() {
  // A v3 snapshot round-trips weather and credentials together.
  Settings a;
  std::strcpy(a.networks[0].ssid, "net");
  a.activeNetwork = 0;
  a.weather.enabled = true;
  std::strcpy(a.weather.name, "Home town");
  a.weather.latE4 = -900000; // extreme, must survive
  a.weather.lonE4 = 1800000;
  settings::serializeBin(a, binBuf);

  Settings b;
  CHECK(settings::deserializeBin(binBuf, sizeof(binBuf), b));
  CHECK(std::strcmp(b.networks[0].ssid, "net") == 0);
  CHECK(b.weather.enabled);
  CHECK(std::strcmp(b.weather.name, "Home town") == 0);
  CHECK(b.weather.latE4 == -900000);
  CHECK(b.weather.lonE4 == 1800000);
}

void testBinVersionLengthMismatchAndCorruptWeather() {
  Settings a;
  std::strcpy(a.networks[0].ssid, "net");
  a.activeNetwork = 0;
  a.weather.enabled = true;
  a.weather.latE4 = 123456;
  settings::serializeBin(a, binBuf);
  Settings b;

  // v3 bytes at a legacy length, and legacy bytes at the v3 length, are
  // both rejected: version and length stay bound 1:1.
  CHECK(!settings::deserializeBin(binBuf, settings::kBinPayloadSizeV1, b));
  CHECK(!settings::deserializeBin(binBuf, settings::kBinPayloadSizeV2, b));
  uint8_t v1[settings::kBinPayloadSizeV1] = {};
  v1[0] = 1;
  std::memcpy(v1 + 2, "x", 2);
  CHECK(settings::deserializeBin(v1, sizeof(v1), b));
  CHECK(!settings::deserializeBin(v1, settings::kBinPayloadSize, b));
  CHECK(!settings::deserializeBin(v1, settings::kBinPayloadSizeV2, b));

  // enabled byte beyond 1 is corruption (v3 weather block).
  settings::serializeBin(a, binBuf);
  binBuf[400] = 2;
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));

  // Out-of-range coordinate is corruption.
  settings::serializeBin(a, binBuf);
  binBuf[418] = 0xFF;
  binBuf[419] = 0xFF;
  binBuf[420] = 0xFF;
  binBuf[421] = 0x7F; // 0x7FFFFFFF latE4
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));

  // Name without a NUL inside its 17-byte window is corruption.
  settings::serializeBin(a, binBuf);
  std::memset(binBuf + 401, 'n', 17);
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));
}

// --- helpers ---------------------------------------------------------------

void testActiveAndAnyHelpers() {
  Settings s;
  CHECK(settings::activeNetwork(s) == nullptr);
  CHECK(!settings::anyNetwork(s));

  std::strcpy(s.networks[1].ssid, "Train");
  CHECK(settings::anyNetwork(s));
  CHECK(settings::activeNetwork(s) == nullptr); // still no active index

  s.activeNetwork = 1;
  CHECK(settings::activeNetwork(s) == &s.networks[1]);

  // An active index naming an empty slot behaves as none.
  s.activeNetwork = 2;
  CHECK(settings::activeNetwork(s) == nullptr);
}

// --- WireGuard block (v4) ------------------------------------------------------

void testJsonWireGuardRoundTrip() {
  Settings a;
  a.wireguard.enabled = true;
  std::strcpy(a.wireguard.endpoint, "wg.example.org");
  a.wireguard.port = 51820;
  std::strcpy(a.wireguard.ownPrivateKey, "yA9sG7Xw0EKsM2rWJm6zDlQHn0B3cYvT8uNkPqLmRjk=");
  std::strcpy(a.wireguard.peerPublicKey, "H4m6cUvK2pN9xQeR7sT0wYbLdJfAz3gViOmCnXkPqLm=");
  std::strcpy(a.wireguard.ownIp, "10.66.66.2");
  a.wireguard.keepalive = 45;
  const size_t n = settings::serialize(a, buf, sizeof(buf));
  CHECK(n > 0);

  Settings b;
  CHECK(settings::deserialize(buf, b));
  CHECK(b.wireguard.enabled);
  CHECK(std::strcmp(b.wireguard.endpoint, "wg.example.org") == 0);
  CHECK(b.wireguard.port == 51820);
  CHECK(std::strcmp(b.wireguard.ownPrivateKey, "yA9sG7Xw0EKsM2rWJm6zDlQHn0B3cYvT8uNkPqLmRjk=") == 0);
  CHECK(std::strcmp(b.wireguard.peerPublicKey, "H4m6cUvK2pN9xQeR7sT0wYbLdJfAz3gViOmCnXkPqLm=") == 0);
  CHECK(std::strcmp(b.wireguard.ownIp, "10.66.66.2") == 0);
  CHECK(b.wireguard.keepalive == 45);
  CHECK(settings::wireGuardConfigured(b.wireguard));
}

void testJsonWireGuardDefaultsAndKeep() {
  // A v3-era document carries no wg block: the defaults survive untouched.
  Settings k;
  std::strcpy(k.wireguard.endpoint, "keepme.example");
  CHECK(settings::deserialize("{\"v\":3,\"ssid\":\"y\"}", k));
  CHECK(std::strcmp(k.wireguard.endpoint, "keepme.example") == 0);
  CHECK(!k.wireguard.enabled);
  CHECK(k.wireguard.keepalive == settings::kWgKeepaliveDefaultSecs);

  // Partial block: absent keys keep the current values.
  CHECK(settings::deserialize("{\"v\":4,\"wg\":{\"en\":1,\"port\":51820}}", k));
  CHECK(k.wireguard.enabled);
  CHECK(k.wireguard.port == 51820);
  CHECK(std::strcmp(k.wireguard.endpoint, "keepme.example") == 0);
}

void testJsonWireGuardRejects() {
  Settings s;
  CHECK(!settings::deserialize("{\"v\":4,\"wg\":\"x\"}", s));
  CHECK(!settings::deserialize("{\"v\":4,\"wg\":{\"port\":65536}}", s));
  CHECK(!settings::deserialize("{\"v\":4,\"wg\":{\"port\":-1}}", s));
  CHECK(!settings::deserialize("{\"v\":4,\"wg\":{\"ka\":301}}", s));
  CHECK(!settings::deserialize("{\"v\":4,\"wg\":{\"en\":7}}", s));
  // A short key is shape-valid (only wireGuardConfigured() demands the full
  // length); overflow is rejected below.
  CHECK(settings::deserialize("{\"v\":4,\"wg\":{\"priv\":\"not base64!\"}}", s));
  CHECK(!settings::wireGuardConfigured(s.wireguard));
  const std::string bigEp(settings::kWgEndpointMaxLen + 1, 'e');
  const std::string big = "{\"v\":4,\"wg\":{\"ep\":\"" + bigEp + "\"}}";
  CHECK(!settings::deserialize(big.c_str(), s));
  const std::string bigKey(settings::kWgKeyLen + 1, 'k');
  const std::string bigPriv = "{\"v\":4,\"wg\":{\"priv\":\"" + bigKey + "\"}}";
  CHECK(!settings::deserialize(bigPriv.c_str(), s));
  CHECK(std::strcmp(s.wireguard.endpoint, "") == 0);
}

void testWireGuardConfiguredHelper() {
  settings::WireGuard w;
  CHECK(!settings::wireGuardConfigured(w)); // all empty
  std::strcpy(w.endpoint, "192.0.2.9");
  w.port = 51820;
  std::memset(w.ownPrivateKey, 'a', settings::kWgKeyLen);
  w.ownPrivateKey[settings::kWgKeyLen] = '\0';
  CHECK(!settings::wireGuardConfigured(w)); // peer key missing
  std::strcpy(w.peerPublicKey, w.ownPrivateKey);
  CHECK(!settings::wireGuardConfigured(w)); // tunnel IP missing
  std::strcpy(w.ownIp, "10.0.0.2");
  CHECK(settings::wireGuardConfigured(w));
  w.port = 0;
  CHECK(!settings::wireGuardConfigured(w)); // port required
}

// Hand-built v3 binary payload (432 B): accepted with the WireGuard block at
// its defaults (v4 migration path).
void testBinV3Migrates() {
  uint8_t v3[settings::kBinPayloadSizeV3] = {};
  v3[0] = 3; // version LE
  v3[2] = 0; // active index 0
  std::memcpy(v3 + 4, "OldHome", 8);
  v3[400] = 1; // weather enabled

  Settings b = sentinelSettings();
  CHECK(settings::deserializeBin(v3, sizeof(v3), b));
  CHECK(std::strcmp(b.networks[0].ssid, "OldHome") == 0);
  CHECK(b.activeNetwork == 0);
  CHECK(b.weather.enabled);
  CHECK(!b.wireguard.enabled);
  CHECK(std::strcmp(b.wireguard.endpoint, "") == 0);
  CHECK(b.wireguard.keepalive == settings::kWgKeepaliveDefaultSecs);
  CHECK(b.schemaVersion == settings::kSchemaVersion);
}

void testBinWireGuardRoundTrip() {
  Settings a;
  std::strcpy(a.networks[0].ssid, "net");
  a.activeNetwork = 0;
  a.wireguard.enabled = true;
  std::strcpy(a.wireguard.endpoint, "vpn.example.net");
  a.wireguard.port = 51820;
  std::strcpy(a.wireguard.ownPrivateKey, "yA9sG7Xw0EKsM2rWJm6zDlQHn0B3cYvT8uNkPqLmRjk=");
  std::strcpy(a.wireguard.peerPublicKey, "H4m6cUvK2pN9xQeR7sT0wYbLdJfAz3gViOmCnXkPqLm=");
  std::strcpy(a.wireguard.ownIp, "10.66.66.7");
  a.wireguard.keepalive = 300; // max
  settings::serializeBin(a, binBuf);

  Settings b;
  CHECK(settings::deserializeBin(binBuf, sizeof(binBuf), b));
  CHECK(b.wireguard.enabled);
  CHECK(std::strcmp(b.wireguard.endpoint, "vpn.example.net") == 0);
  CHECK(b.wireguard.port == 51820);
  CHECK(std::strcmp(b.wireguard.ownPrivateKey, "yA9sG7Xw0EKsM2rWJm6zDlQHn0B3cYvT8uNkPqLmRjk=") == 0);
  CHECK(std::strcmp(b.wireguard.peerPublicKey, "H4m6cUvK2pN9xQeR7sT0wYbLdJfAz3gViOmCnXkPqLm=") == 0);
  CHECK(std::strcmp(b.wireguard.ownIp, "10.66.66.7") == 0);
  CHECK(b.wireguard.keepalive == 300);
}

void testBinWireGuardCorrupt() {
  Settings a;
  std::strcpy(a.networks[0].ssid, "net");
  a.activeNetwork = 0;
  Settings b;

  // enabled byte beyond 1 is corruption.
  settings::serializeBin(a, binBuf);
  binBuf[432] = 2;
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));

  // keepalive beyond the maximum is corruption (u16 LE at 606).
  settings::serializeBin(a, binBuf);
  binBuf[606] = 45; // 0x012D = 301 LE
  binBuf[607] = 1;
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));

  // endpoint without a NUL inside its 65-byte window is corruption.
  settings::serializeBin(a, binBuf);
  std::memset(binBuf + 433, 'e', 65);
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));

  // v4 bytes at the v3 length and v3 bytes at the v4 length are rejected.
  settings::serializeBin(a, binBuf);
  CHECK(!settings::deserializeBin(binBuf, settings::kBinPayloadSizeV3, b));
  uint8_t v3[settings::kBinPayloadSizeV3] = {};
  v3[0] = 3;
  std::memcpy(v3 + 4, "x", 2);
  CHECK(!settings::deserializeBin(v3, settings::kBinPayloadSize, b));
}

// --- AC block (v5) -----------------------------------------------------------

void testJsonAcRoundTrip() {
  Settings a;
  a.ac.beep = false;
  a.ac.buttonLock = true;
  const size_t n = settings::serialize(a, buf, sizeof(buf));
  CHECK(n > 0);
  Settings b = sentinelSettings();
  CHECK(settings::deserialize(buf, b));
  CHECK(!b.ac.beep);
  CHECK(b.ac.buttonLock);
}

void testJsonAcDefaultsAndKeep() {
  // Defaults are the shipped behavior: beep on, lock off.
  Settings d;
  CHECK(d.ac.beep);
  CHECK(!d.ac.buttonLock);

  // A v4-era document carries no ac block: the current values stay untouched.
  Settings k;
  k.ac.beep = false;
  CHECK(settings::deserialize("{\"v\":4,\"ssid\":\"y\"}", k));
  CHECK(!k.ac.beep);
  CHECK(!k.ac.buttonLock);

  // Partial block: a present key overwrites, an absent one keeps.
  CHECK(settings::deserialize("{\"v\":5,\"ac\":{\"beep\":1}}", k));
  CHECK(k.ac.beep);
  CHECK(!k.ac.buttonLock);
}

void testJsonAcRejects() {
  Settings s;
  CHECK(!settings::deserialize("{\"v\":5,\"ac\":\"x\"}", s));
  CHECK(!settings::deserialize("{\"v\":5,\"ac\":{\"beep\":7}}", s));
  CHECK(!settings::deserialize("{\"v\":5,\"ac\":{\"lock\":\"on\"}}", s));
}

// Hand-built v4 binary payload (608 B): accepted with the AC block at its
// defaults (v5 migration path).
void testBinV4Migrates() {
  uint8_t v4[settings::kBinPayloadSizeV4] = {};
  v4[0] = 4; // version LE
  v4[2] = 0; // active index 0
  std::memcpy(v4 + 4, "OldNet", 7);

  Settings b = sentinelSettings();
  CHECK(settings::deserializeBin(v4, sizeof(v4), b));
  CHECK(std::strcmp(b.networks[0].ssid, "OldNet") == 0);
  CHECK(b.ac.beep);
  CHECK(!b.ac.buttonLock);
  CHECK(b.schemaVersion == settings::kSchemaVersion);
}

void testBinAcRoundTrip() {
  Settings a;
  std::strcpy(a.networks[0].ssid, "net");
  a.activeNetwork = 0;
  a.ac.beep = false;
  a.ac.buttonLock = true;
  settings::serializeBin(a, binBuf);

  Settings b;
  CHECK(settings::deserializeBin(binBuf, sizeof(binBuf), b));
  CHECK(!b.ac.beep);
  CHECK(b.ac.buttonLock);

  // The defaults round-trip as 1/0 bytes, not as leftovers.
  Settings d;
  settings::serializeBin(d, binBuf);
  CHECK(binBuf[608] == 1);
  CHECK(binBuf[609] == 0);
  Settings e = sentinelSettings();
  CHECK(settings::deserializeBin(binBuf, sizeof(binBuf), e));
  CHECK(e.ac.beep);
  CHECK(!e.ac.buttonLock);
}

void testBinAcCorrupt() {
  Settings b;
  // beep byte beyond 1 is corruption (u8 at 608).
  settings::serializeBin(Settings{}, binBuf);
  binBuf[608] = 2;
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));

  // buttonLock byte beyond 1 is corruption (u8 at 609).
  settings::serializeBin(Settings{}, binBuf);
  binBuf[609] = 2;
  CHECK(!settings::deserializeBin(binBuf, sizeof(binBuf), b));

  // v5 bytes at the v4 length and v4 bytes at the v5 length are rejected.
  settings::serializeBin(Settings{}, binBuf);
  CHECK(!settings::deserializeBin(binBuf, settings::kBinPayloadSizeV4, b));
  uint8_t v4[settings::kBinPayloadSizeV4] = {};
  v4[0] = 4;
  std::memcpy(v4 + 4, "x", 2);
  CHECK(!settings::deserializeBin(v4, settings::kBinPayloadSize, b));
}

} // namespace

int main() {
  testDefaultRoundTrip();
  testValuesRoundTrip();
  testMultiNetworkRoundTrip();
  testBoundaryLengths();
  testSerializedMaxSizeBound();
  testUnicodeEscapeDecodes();
  testV2JsonMigratesToProfileZero();
  testAbsentKeysKeepValues();
  testRejectsGarbage();
  testRejectsBadVersion();
  testRejectsBadFields();
  testRejectsBadNetworkList();
  testTooSmallBuffer();
  testBinRoundTrip();
  testBinFullSnapshotOverwrite();
  testBinBoundaryLengths();
  testBinRejectsCorrupt();
  testJsonWeatherRoundTrip();
  testJsonWeatherDefaultsAndKeep();
  testJsonWeatherRejects();
  testBinV1Migrates();
  testBinV2Migrates();
  testBinV2RoundTripThroughV3();
  testBinVersionLengthMismatchAndCorruptWeather();
  testJsonWireGuardRoundTrip();
  testJsonWireGuardDefaultsAndKeep();
  testJsonWireGuardRejects();
  testWireGuardConfiguredHelper();
  testBinV3Migrates();
  testBinWireGuardRoundTrip();
  testBinWireGuardCorrupt();
  testJsonAcRoundTrip();
  testJsonAcDefaultsAndKeep();
  testJsonAcRejects();
  testBinV4Migrates();
  testBinAcRoundTrip();
  testBinAcCorrupt();
  testActiveAndAnyHelpers();

  if (failures == 0) {
    std::printf("settings: all checks passed\n");
    return 0;
  }
  std::printf("settings: %d checks FAILED\n", failures);
  return 1;
}
