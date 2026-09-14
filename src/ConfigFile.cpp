#include "ConfigFile.h"

#include <SDCardManager.h>
#include <esp_system.h>

#include <Devices.h>

namespace configfile {

namespace {

// Scratch sized for the largest envelope any kind uses (devices). Shared
// statics: load/save are main-loop only and never concurrent.
constexpr size_t kMaxPayload = devices::kBinPayloadSize;
constexpr size_t kEnvelopeMax = vault::sealSize(kMaxPayload);
// readFileToBuffer reserves one byte of its size argument for a NUL; without a
// slot beyond the largest envelope, an exact-size file truncates by one byte
// and the CRC check fails the intact file as corrupt.
uint8_t sEnvelope[kEnvelopeMax + 1];
uint8_t sPlain[devices::kBinPayloadSize];

} // namespace

Status load(const uint8_t* key, vault::Kind kind, const char* path, Consumer consume, void* ctx) {
  if (!SdMan.exists(path)) return Status::kMissing;
  const size_t len = SdMan.readFileToBuffer(path, reinterpret_cast<char*>(sEnvelope), kEnvelopeMax + 1, kEnvelopeMax);
  if (len == 0) return Status::kMissing;

  size_t plainLen = 0;
  switch (vault::open(kind, key, sEnvelope, len, sPlain, sizeof(sPlain), &plainLen)) {
    case vault::OpenError::kNone:
      break;
    case vault::OpenError::kTooShort:
      return Status::kMissing; // empty/truncated remnant
    case vault::OpenError::kCorrupt:
      return Status::kCorrupt;
    case vault::OpenError::kInternal:
      return Status::kError;
    case vault::OpenError::kBadMagic:
    case vault::OpenError::kBadKind:
    case vault::OpenError::kBadFormat:
      return Status::kForeign;
  }
  // Right envelope, rejected payload: wrong device key or a firmware-version
  // skew — the caller must not overwrite these files blindly.
  if (!consume(sPlain, plainLen, ctx)) return Status::kForeign;
  return Status::kLoaded;
}

bool save(const uint8_t* key, vault::Kind kind, const char* path, const uint8_t* plain, size_t plainLen) {
  uint8_t iv[vault::kIvLen];
  esp_fill_random(iv, sizeof(iv));
  const size_t sealed = vault::seal(kind, key, iv, plain, plainLen, sEnvelope, sizeof(sEnvelope));
  if (sealed == 0) return false;

  FsFile file;
  if (!SdMan.openFileForWrite("VAULT", path, file)) return false;
  const size_t written = file.write(sEnvelope, sealed);
  // SdFat write() only counts bytes into its cache; sync()/close() are what
  // commit to the medium, so their results decide durability. A close-time
  // flush failure must fail the save, not silently lose it.
  const bool flushed = file.sync();
  const bool closed = file.close();
  if (written != sealed || !flushed || !closed) {
    Serial.printf("[VAULT] save failed %s (written %u/%u sync=%d close=%d)\n", path, static_cast<unsigned>(written),
                  static_cast<unsigned>(sealed), static_cast<int>(flushed), static_cast<int>(closed));
    return false;
  }
  return true;
}

Status loadAb(const uint8_t* key, vault::Kind kind, const char* primary, const char* backup, Consumer consume,
              void* ctx, bool* usedBackup) {
  *usedBackup = false;
  const Status first = load(key, kind, primary, consume, ctx);
  if (first == Status::kLoaded) return first;

  const Status second = load(key, kind, backup, consume, ctx);
  if (second == Status::kLoaded) {
    *usedBackup = true;
    return second;
  }
  if (first == Status::kForeign || second == Status::kForeign) return Status::kForeign;
  return first != Status::kMissing ? first : second;
}

} // namespace configfile
