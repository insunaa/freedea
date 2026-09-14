// Host unit tests for lib/Devices: binary payload round-trips, strict
// rejection of corrupt payloads, and the device.json import parser.

#include "Devices.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                      \
      ++failures;                                                                                                      \
    }                                                                                                                  \
  } while (0)

using devices::Device;
using devices::ImportError;
using devices::List;

alignas(4) uint8_t binBuf[devices::kBinPayloadSize];

List sampleList() {
  List list;
  list.count = 2;
  Device& a = list.devices[0];
  a.id = 15393162840672ULL;
  a.version = 3;
  a.port = 6444;
  a.ip[0] = 192;
  a.ip[1] = 168;
  a.ip[2] = 1;
  a.ip[3] = 50;
  std::strcpy(a.name, "Living room");
  a.tokenLen = 128;
  for (size_t i = 0; i < 128; ++i)
    a.token[i] = static_cast<uint8_t>(i);
  for (size_t i = 0; i < devices::kKeyLen; ++i)
    a.key[i] = static_cast<uint8_t>(0xA0 + i);

  Device& b = list.devices[1];
  b.id = 7;
  b.version = 0;
  b.port = 6445;
  std::strcpy(b.name, "Bedroom");
  b.tokenLen = 0;
  return list;
}

void testBinRoundTrip() {
  const List a = sampleList();
  devices::serializeBin(a, binBuf);

  List b;
  CHECK(devices::deserializeBin(binBuf, sizeof(binBuf), b));
  CHECK(b.count == 2);
  for (int i = 0; i < 2; ++i) {
    const Device& x = a.devices[i];
    const Device& y = b.devices[i];
    CHECK(x.id == y.id);
    CHECK(x.version == y.version);
    CHECK(x.port == y.port);
    CHECK(std::memcmp(x.ip, y.ip, 4) == 0);
    CHECK(std::strcmp(x.name, y.name) == 0);
    CHECK(x.tokenLen == y.tokenLen);
    CHECK(std::memcmp(x.token, y.token, x.tokenLen) == 0);
    CHECK(std::memcmp(x.key, y.key, sizeof(x.key)) == 0);
  }
}

void testBinEmptyRoundTrip() {
  const List empty;
  devices::serializeBin(empty, binBuf);
  List b = sampleList();
  CHECK(devices::deserializeBin(binBuf, sizeof(binBuf), b));
  CHECK(b.count == 0);
}

void testBinRejectsCorrupt() {
  const List a = sampleList();
  List out;

  devices::serializeBin(a, binBuf);
  CHECK(!devices::deserializeBin(binBuf, sizeof(binBuf) - 1, out));

  devices::serializeBin(a, binBuf);
  binBuf[0] ^= 0xFF; // payload version
  CHECK(!devices::deserializeBin(binBuf, sizeof(binBuf), out));

  devices::serializeBin(a, binBuf);
  binBuf[2] = 0xFF; // count
  CHECK(!devices::deserializeBin(binBuf, sizeof(binBuf), out));

  devices::serializeBin(a, binBuf);
  std::memset(binBuf + 4, 0, 8); // id = 0
  CHECK(!devices::deserializeBin(binBuf, sizeof(binBuf), out));

  devices::serializeBin(a, binBuf);
  binBuf[4 + 0 * devices::kRecordSize + 8] = 5; // unknown protocol version
  CHECK(!devices::deserializeBin(binBuf, sizeof(binBuf), out));

  devices::serializeBin(a, binBuf);
  binBuf[4 + 0 * devices::kRecordSize + 40] = 0xFF; // tokenLen > max
  CHECK(!devices::deserializeBin(binBuf, sizeof(binBuf), out));

  devices::serializeBin(a, binBuf);
  std::memset(binBuf + 4 + 0 * devices::kRecordSize + 16, 'X', devices::kNameCapacity); // name w/o NUL
  CHECK(!devices::deserializeBin(binBuf, sizeof(binBuf), out));

  devices::serializeBin(a, binBuf);
  // Record 1 duplicates record 0's id.
  std::memcpy(binBuf + 4 + devices::kRecordSize, binBuf + 4, 8);
  CHECK(!devices::deserializeBin(binBuf, sizeof(binBuf), out));
}

void testImportFull() {
  static const char* kJson =
      "{\"v\":1,\"devices\":["
      "{\"id\":15393162840672,\"name\":\"Living room\",\"version\":3,\"ip\":\"192.168.1.50\",\"port\":6444,"
      "\"token\":\"000102030405060708090A0B0C0D0E0F\",\"key\":"
      "\"00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF\"},"
      "{\"id\":\"4242\",\"port\":6000}]}";
  List out;
  CHECK(devices::parseImportJson(kJson, out) == ImportError::kNone);
  CHECK(out.count == 2);
  CHECK(out.devices[0].id == 15393162840672ULL);
  CHECK(std::strcmp(out.devices[0].name, "Living room") == 0);
  CHECK(out.devices[0].version == 3);
  CHECK(out.devices[0].ip[0] == 192 && out.devices[0].ip[3] == 50);
  CHECK(out.devices[0].port == 6444);
  CHECK(out.devices[0].tokenLen == 16);
  CHECK(out.devices[0].token[1] == 0x01 && out.devices[0].token[15] == 0x0F);
  CHECK(out.devices[0].key[31] == 0xFF);
  CHECK(out.devices[1].id == 4242); // decimal string id
  CHECK(std::strcmp(out.devices[1].name, "AC-1092") == 0);
  CHECK(out.devices[1].version == 0);
  CHECK(!out.devices[1].hasCredentials());

  // Import -> bin -> import-equivalent.
  devices::serializeBin(out, binBuf);
  List again;
  CHECK(devices::deserializeBin(binBuf, sizeof(binBuf), again));
  CHECK(again.count == out.count);
  CHECK(again.devices[0].id == out.devices[0].id);
  CHECK(again.devices[0].tokenLen == out.devices[0].tokenLen);
  CHECK(std::memcmp(again.devices[0].token, out.devices[0].token, 16) == 0);
}

void testImportHexIdString() {
  static const char* kJson = "{\"v\":1,\"devices\":[{\"id\":\"0xF7B4\"}]}";
  List out;
  CHECK(devices::parseImportJson(kJson, out) == ImportError::kNone);
  CHECK(out.count == 1);
  CHECK(out.devices[0].id == 0xF7B4);
  CHECK(std::strcmp(out.devices[0].name, "AC-F7B4") == 0);
}

void listUnchangedOnFailure() {
  static const char* kGood = "{\"v\":1,\"devices\":[{\"id\":9}]}";
  List out;
  CHECK(devices::parseImportJson(kGood, out) == ImportError::kNone);
  CHECK(out.count == 1);
  CHECK(devices::parseImportJson("{\"v\":1,\"devices\":[{\"id\":0}]}", out) == ImportError::kBadId);
  CHECK(out.count == 1); // untouched
  CHECK(out.devices[0].id == 9);
}

// One-device document helper with a single tweakable fragment.
char jsonBuf[1024];
ImportError importOne(const char* fieldFragment) {
  std::snprintf(jsonBuf, sizeof(jsonBuf), "{\"v\":1,\"devices\":[{\"id\":9%s}]}", fieldFragment);
  List out;
  return devices::parseImportJson(jsonBuf, out);
}

void testImportRejections() {
  List out;
  CHECK(devices::parseImportJson("nope", out) == ImportError::kJson);
  CHECK(devices::parseImportJson("[1,2]", out) == ImportError::kJson);
  CHECK(devices::parseImportJson("{\"v\":1}", out) == ImportError::kJson);
  CHECK(devices::parseImportJson("{\"devices\":[]}", out) == ImportError::kVersion);
  CHECK(devices::parseImportJson("{\"v\":2,\"devices\":[]}", out) == ImportError::kVersion);
  CHECK(devices::parseImportJson("{\"v\":1,\"devices\":[{\"id\":1},{\"id\":2},{\"id\":3},{\"id\":4},{\"id\":5}]}",
                                 out) == ImportError::kTooMany);
  CHECK(devices::parseImportJson("{\"v\":1,\"devices\":[{\"id\":5},{\"id\":5}]}", out) == ImportError::kBadId);
  CHECK(devices::parseImportJson("{\"v\":1,\"devices\":[{\"id\":\"x\"}]}", out) == ImportError::kBadId);

  CHECK(importOne(",\"name\":\"\"") == ImportError::kBadName);
  CHECK(importOne(",\"name\":\"aaaaaaaaaaaaaaaaaaaaaaaa\"") == ImportError::kBadName); // 24 chars
  CHECK(importOne(",\"name\":\"caf\xc3\xa9\"") == ImportError::kBadName);              // non-ASCII UTF-8
  CHECK(importOne(",\"version\":4") == ImportError::kBadVersion);
  CHECK(importOne(",\"ip\":\"1.2.3\"") == ImportError::kBadIp);
  CHECK(importOne(",\"ip\":\"1.2.3.256\"") == ImportError::kBadIp);
  CHECK(importOne(",\"port\":0") == ImportError::kBadPort);
  CHECK(importOne(",\"port\":65536") == ImportError::kBadPort);
  CHECK(importOne(",\"token\":\"abc\"") == ImportError::kBadToken);               // odd length
  CHECK(importOne(",\"token\":\"zz\"") == ImportError::kBadToken);                // non-hex
  CHECK(importOne(",\"token\":\"\"") == ImportError::kBadToken);                  // empty
  CHECK(importOne(",\"key\":\"00\"") == ImportError::kBadToken);                  // key without token
  CHECK(importOne(",\"token\":\"AABB\"") == ImportError::kBadKey);                // token without key
  CHECK(importOne(",\"token\":\"AABB\",\"key\":\"00\"") == ImportError::kBadKey); // short key
  // Token at exactly the 128-byte cap and key at exactly 32 B parse:
  char tokenHex[2 * devices::kTokenMaxLen + 1];
  std::memset(tokenHex, 'a', 2 * devices::kTokenMaxLen);
  tokenHex[2 * devices::kTokenMaxLen] = '\0';
  char keyHex[2 * devices::kKeyLen + 1];
  std::memset(keyHex, 'b', 2 * devices::kKeyLen);
  keyHex[2 * devices::kKeyLen] = '\0';
  char frag[4 * (devices::kTokenMaxLen + devices::kKeyLen) + 64];
  std::snprintf(frag, sizeof(frag), ",\"token\":\"%s\",\"key\":\"%s\"", tokenHex, keyHex);
  CHECK(importOne(frag) == ImportError::kNone);
  // One hex char over the cap is rejected:
  tokenHex[2 * devices::kTokenMaxLen] = 'a';
  tokenHex[2 * devices::kTokenMaxLen + 1] = '\0';
  std::snprintf(frag, sizeof(frag), ",\"token\":\"%s\",\"key\":\"%s\"", tokenHex, keyHex);
  CHECK(importOne(frag) == ImportError::kBadToken);
}

void testImportEmptyArrayOk() {
  List out = sampleList();
  CHECK(devices::parseImportJson("{\"v\":1,\"devices\":[]}", out) == ImportError::kNone);
  CHECK(out.count == 0);
}

} // namespace

int main() {
  testBinRoundTrip();
  testBinEmptyRoundTrip();
  testBinRejectsCorrupt();
  testImportFull();
  testImportHexIdString();
  listUnchangedOnFailure();
  testImportRejections();
  testImportEmptyArrayOk();

  if (failures == 0) {
    std::printf("devices: all checks passed\n");
    return 0;
  }
  std::printf("devices: %d checks FAILED\n", failures);
  return 1;
}
