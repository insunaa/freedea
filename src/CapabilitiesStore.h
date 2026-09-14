#pragma once

// Capabilities cache (4.5): the AcService capabilities set for the current
// target, cached to /.freedea/caps.bin so a reboot skips the GetCapabilities
// query. Plain binary, not a Vault envelope: the cache holds no secrets and
// the recovery for any anomaly is simply "query again", so the CRC'd
// single-entry file keeps its own header instead of A/B slots.
//
// File layout (little-endian, all multi-byte fields byte-copied):
//   magic 'F','C','P','1' | version u8 | deviceId u64 | payloadLen u16
//   crc32 u32 over the payload | payload = raw AcCapabilities bytes
// payloadLen is sizeof(midea::AcCapabilities), so a firmware build with a
// different struct layout invalidates the cache instead of misreading it.

#include <cstdint>

#include <Responses.h>

class CapabilitiesStore {
public:
  // Mirrors SettingsStore::init(); without SD the cache is inert (always miss,
  // saves no-op).
  void init(bool sdReady);

  // Loads the cache entry for deviceId. True only when the file validates and
  // carries this device's entry; anything else (missing, corrupt, foreign
  // device, layout skew) is a miss and gets overwritten on the next save.
  bool load(uint64_t deviceId, midea::AcCapabilities& out) const;

  // Writes the single-entry cache, replacing whatever was there.
  bool save(uint64_t deviceId, const midea::AcCapabilities& caps);

private:
  static constexpr const char* kDirPath = "/.freedea";
  static constexpr const char* kPath = "/.freedea/caps.bin";
  bool sdReady_ = false;
};
