// Host unit tests for lib/Vault: envelope roundtrips, corruption detection,
// key-derivation KAT (generated with Python hashlib), CRC32 KAT (zlib).

#include "Vault.h"

#include <cstdio>
#include <cstring>

namespace {

int gFailures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++gFailures;
  }
}

// sha256("freedea:vault:v1" || a0b1c2d3e4f5), Python hashlib.
const uint8_t kMac[6] = {0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5};
const uint8_t kExpectedKey[32] = {0x24, 0x95, 0x76, 0xf2, 0x67, 0x15, 0x3c, 0xae, 0xeb, 0xd2, 0x88,
                                  0x2f, 0xa1, 0x86, 0x04, 0xf0, 0xb2, 0xad, 0xa8, 0xd0, 0xb6, 0x29,
                                  0x5b, 0x2e, 0x10, 0xb7, 0xcf, 0xdb, 0x9c, 0x91, 0xa9, 0xc6};

const uint8_t kKey[32] = {1};
const uint8_t kOtherKey[32] = {2};
const uint8_t kIv[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                         0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};

constexpr size_t kMaxPayload = 300; // exercises the multi-block path
uint8_t sSealed[vault::sealSize(kMaxPayload)];
uint8_t sOpen[vault::sealSize(kMaxPayload)];

size_t sealTestPayload(uint8_t* out, size_t outCap, const void* plain, size_t plainLen) {
  return vault::seal(vault::Kind::kSettings, kKey, kIv, static_cast<const uint8_t*>(plain), plainLen, out, outCap);
}

void testDeriveKeyKAT() {
  uint8_t key[32];
  vault::deriveKey(kMac, key);
  check(std::memcmp(key, kExpectedKey, 32) == 0, "deriveKey matches hashlib KAT");

  const uint8_t mac2[6] = {0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF6};
  uint8_t key2[32];
  vault::deriveKey(mac2, key2);
  check(std::memcmp(key, key2, 32) != 0, "deriveKey differs per MAC");
}

void testCrcKAT() {
  check(vault::crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u, "crc32 matches zlib KAT");
  check(vault::crc32(nullptr, 0) == 0u, "crc32 of empty input is 0");
}

void testRoundtrip() {
  const char payload[] = "hello freedea"; // 13 bytes -> padded to 16
  const size_t sealed = sealTestPayload(sSealed, sizeof(sSealed), payload, sizeof(payload));
  check(sealed == vault::sealSize(sizeof(payload)), "seal returns expected size");
  check(std::memcmp(sSealed + 16, kIv, 16) == 0, "iv stored in header");

  size_t plainLen = 0;
  const auto err = vault::open(vault::Kind::kSettings, kKey, sSealed, sealed, sOpen, sizeof(sOpen), &plainLen);
  check(err == vault::OpenError::kNone, "roundtrip opens");
  check(plainLen == sizeof(payload), "roundtrip restores exact length");
  check(std::memcmp(sOpen, payload, sizeof(payload)) == 0, "roundtrip restores bytes");
}

void testRoundtripEmptyAndLarge() {
  size_t sealed = sealTestPayload(sSealed, sizeof(sSealed), nullptr, 0);
  check(sealed == vault::kHeaderLen, "empty payload seals to header only");
  size_t plainLen = 1;
  check(vault::open(vault::Kind::kSettings, kKey, sSealed, sealed, sOpen, sizeof(sOpen), &plainLen) ==
            vault::OpenError::kNone,
        "empty payload opens");
  check(plainLen == 0, "empty payload length 0");

  uint8_t big[kMaxPayload];
  for (size_t i = 0; i < sizeof(big); ++i)
    big[i] = static_cast<uint8_t>(i * 7 + 3);
  sealed = sealTestPayload(sSealed, sizeof(sSealed), big, sizeof(big));
  check(sealed == vault::sealSize(sizeof(big)), "multi-block seal size");
  plainLen = 0;
  check(vault::open(vault::Kind::kSettings, kKey, sSealed, sealed, sOpen, sizeof(sOpen), &plainLen) ==
            vault::OpenError::kNone,
        "multi-block opens");
  check(plainLen == sizeof(big) && std::memcmp(sOpen, big, sizeof(big)) == 0, "multi-block bytes match");
}

void testCorruptionDetected() {
  const char payload[] = "integrity sample";
  const size_t sealed = sealTestPayload(sSealed, sizeof(sSealed), payload, sizeof(payload));

  uint8_t buf[sizeof(sSealed)];
  size_t plainLen = 0;

  // Flip one ciphertext byte.
  std::memcpy(buf, sSealed, sealed);
  buf[vault::kHeaderLen] ^= 0x01;
  check(vault::open(vault::Kind::kSettings, kKey, buf, sealed, sOpen, sizeof(sOpen), &plainLen) ==
            vault::OpenError::kCorrupt,
        "ciphertext bit flip -> kCorrupt");

  // Flip one iv byte (crc covers the iv).
  std::memcpy(buf, sSealed, sealed);
  buf[16] ^= 0x80;
  check(vault::open(vault::Kind::kSettings, kKey, buf, sealed, sOpen, sizeof(sOpen), &plainLen) ==
            vault::OpenError::kCorrupt,
        "iv bit flip -> kCorrupt");

  // Truncate to non-block ciphertext.
  check(vault::open(vault::Kind::kSettings, kKey, sSealed, sealed - 1, sOpen, sizeof(sOpen), &plainLen) ==
            vault::OpenError::kCorrupt,
        "truncated ciphertext -> kCorrupt");
  check(vault::open(vault::Kind::kSettings, kKey, sSealed, vault::kHeaderLen - 1, sOpen, sizeof(sOpen), &plainLen) ==
            vault::OpenError::kTooShort,
        "sub-header input -> kTooShort");
  check(vault::open(vault::Kind::kSettings, kKey, sSealed, 0, sOpen, sizeof(sOpen), &plainLen) ==
            vault::OpenError::kTooShort,
        "empty input -> kTooShort");

  // payloadLen larger than the ciphertext.
  std::memcpy(buf, sSealed, sealed);
  buf[8] = 0xFF;
  buf[9] = 0xFF;
  check(vault::open(vault::Kind::kSettings, kKey, buf, sealed, sOpen, sizeof(sOpen), &plainLen) ==
            vault::OpenError::kCorrupt,
        "payloadLen > ciphertext -> kCorrupt");
}

void testHeaderRejections() {
  const char payload[] = "x";
  const size_t sealed = sealTestPayload(sSealed, sizeof(sSealed), payload, sizeof(payload));

  uint8_t buf[sizeof(sSealed)];
  size_t plainLen = 0;

  std::memcpy(buf, sSealed, sealed);
  buf[0] = 'X';
  check(vault::open(vault::Kind::kSettings, kKey, buf, sealed, sOpen, sizeof(sOpen), &plainLen) ==
            vault::OpenError::kBadMagic,
        "bad magic -> kBadMagic");

  std::memcpy(buf, sSealed, sealed);
  buf[5] = 0x7E;
  check(vault::open(vault::Kind::kSettings, kKey, buf, sealed, sOpen, sizeof(sOpen), &plainLen) ==
            vault::OpenError::kBadFormat,
        "unknown format -> kBadFormat");

  check(vault::open(vault::Kind::kDevices, kKey, sSealed, sealed, sOpen, sizeof(sOpen), &plainLen) ==
            vault::OpenError::kBadKind,
        "wrong kind -> kBadKind");
}

void testWrongKeyDecryptsToGarbage() {
  // A foreign SD card (other device's MAC key): the envelope is structurally
  // valid, so open() succeeds and the payload version check at the caller
  // rejects it. Pin that contract here.
  const char payload[] = "secret-settings-payload-1234";
  const size_t sealed = sealTestPayload(sSealed, sizeof(sSealed), payload, sizeof(payload));

  size_t plainLen = 0;
  const auto err = vault::open(vault::Kind::kSettings, kOtherKey, sSealed, sealed, sOpen, sizeof(sOpen), &plainLen);
  check(err == vault::OpenError::kNone, "wrong key still passes envelope checks");
  check(plainLen == sizeof(payload), "wrong key: length survives (CBC block mode)");
  check(std::memcmp(sOpen, payload, sizeof(payload)) != 0, "wrong key: payload does not match");
}

void testArgumentValidation() {
  const char payload[] = "x";
  check(sealTestPayload(sSealed, vault::kHeaderLen - 1, payload, sizeof(payload)) == 0, "seal into too-small cap -> 0");
  check(vault::seal(vault::Kind::kSettings, kKey, kIv, nullptr, 5, sSealed, sizeof(sSealed)) == 0,
        "seal null plain with length -> 0");
  size_t plainLen = 0;
  check(vault::open(vault::Kind::kSettings, kKey, sSealed, 100, sOpen, 3, &plainLen) == vault::OpenError::kCorrupt,
        "open into too-small cap -> kCorrupt (if header valid)");
}

} // namespace

int main() {
  testDeriveKeyKAT();
  testCrcKAT();
  testRoundtrip();
  testRoundtripEmptyAndLarge();
  testCorruptionDetected();
  testHeaderRejections();
  testWrongKeyDecryptsToGarbage();
  testArgumentValidation();

  if (gFailures) {
    std::printf("%d check(s) FAILED\n", gFailures);
    return 1;
  }
  std::printf("all vault checks passed\n");
  return 0;
}
