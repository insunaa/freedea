#include "CapabilitiesStore.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <cstring>

#include <Vault.h>

namespace {

constexpr uint8_t kMagic[] = {'F', 'C', 'P', '1'};
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderLen = sizeof(kMagic) + 1 + 8 + 2 + 4; // magic ver id len crc
constexpr size_t kFileMax = kHeaderLen + sizeof(midea::AcCapabilities);

void putU64(uint8_t* p, uint64_t v) {
  for (uint8_t i = 0; i < 8; ++i)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (uint8_t i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

uint16_t getU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

void putU32(uint8_t* p, uint32_t v) {
  for (uint8_t i = 0; i < 4; ++i)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t getU32(const uint8_t* p) {
  uint32_t v = 0;
  for (uint8_t i = 0; i < 4; ++i)
    v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}

} // namespace

void CapabilitiesStore::init(bool sdReady) {
  sdReady_ = sdReady;
}

bool CapabilitiesStore::load(uint64_t deviceId, midea::AcCapabilities& out) const {
  if (!sdReady_) return false;

  // Static: readFileToBuffer reserves a NUL slot, hence +1; main-loop only.
  static uint8_t sFile[kFileMax + 1];
  const size_t len = SdMan.readFileToBuffer(kPath, reinterpret_cast<char*>(sFile), sizeof(sFile), kFileMax);
  if (len == 0) return false; // missing or read error
  if (len < kHeaderLen || std::memcmp(sFile, kMagic, sizeof(kMagic)) != 0 || sFile[sizeof(kMagic)] != kVersion) {
    Serial.println("[CAP ] cache unusable (magic/version)");
    return false;
  }
  const uint64_t cachedId = getU64(sFile + sizeof(kMagic) + 1);
  const uint16_t payloadLen = getU16(sFile + sizeof(kMagic) + 1 + 8);
  if (payloadLen != sizeof(midea::AcCapabilities) || len != kHeaderLen + payloadLen) {
    // Firmware layout skew or truncation: re-query instead of misreading.
    Serial.println("[CAP ] cache stale (layout)");
    return false;
  }
  const uint8_t* payload = sFile + kHeaderLen;
  if (getU32(sFile + sizeof(kMagic) + 1 + 8 + 2) != vault::crc32(payload, payloadLen)) {
    Serial.println("[CAP ] cache corrupt (crc)");
    return false;
  }
  if (cachedId != deviceId) {
    Serial.println("[CAP ] cache is for another device");
    return false;
  }
  std::memcpy(&out, payload, payloadLen);
  Serial.printf("[CAP ] loaded cache for id=%llu\n", static_cast<unsigned long long>(cachedId));
  return true;
}

bool CapabilitiesStore::save(uint64_t deviceId, const midea::AcCapabilities& caps) {
  if (!sdReady_) return false;

  static uint8_t sFile[kFileMax];
  size_t n = 0;
  std::memcpy(sFile + n, kMagic, sizeof(kMagic));
  n += sizeof(kMagic);
  sFile[n++] = kVersion;
  putU64(sFile + n, deviceId);
  n += 8;
  putU16(sFile + n, static_cast<uint16_t>(sizeof(midea::AcCapabilities)));
  n += 2;
  putU32(sFile + n, vault::crc32(reinterpret_cast<const uint8_t*>(&caps), sizeof(caps)));
  n += 4;
  std::memcpy(sFile + n, &caps, sizeof(caps));
  n += sizeof(caps);

  if (!SdMan.ensureDirectoryExists(kDirPath)) {
    Serial.println("[CAP ] cannot create /.freedea");
    return false;
  }
  FsFile file;
  if (!SdMan.openFileForWrite("CAP", kPath, file)) return false;
  const size_t written = file.write(sFile, n);
  // SdFat write() only counts into its cache; sync()/close() decide durability.
  const bool flushed = file.sync();
  const bool closed = file.close();
  if (written != n || !flushed || !closed) {
    Serial.printf("[CAP ] save failed (written %u/%u sync=%d close=%d)\n", static_cast<unsigned>(written),
                  static_cast<unsigned>(n), static_cast<int>(flushed), static_cast<int>(closed));
    return false;
  }
  Serial.printf("[CAP ] saved cache for id=%llu\n", static_cast<unsigned long long>(deviceId));
  return true;
}
