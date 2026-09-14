#pragma once

// SD-file plumbing for vault-protected config files (3.4): one file per
// payload kind plus a ".bak" twin; saves always write both so a torn or
// corrupted copy can be detected (envelope CRC) and repaired from the other.
// Firmware-only (SdMan, esp_fill_random). Single-task: the scratch buffers
// are shared statics — call only from the main loop.

#include <Vault.h>

#include <cstddef>
#include <cstdint>

namespace configfile {

enum class Status : uint8_t {
  kLoaded,  // envelope verified, payload consumed
  kMissing, // file absent (or too small to be one)
  kCorrupt, // CRC/structure failure — repairable from the other slot
  kForeign, // not our envelope, or right structure but payload rejects (wrong key or version skew) — never auto-repair
  kError,   // AES or plumbing failure
};

// Consumes the decrypted payload (copies it into the caller's struct);
// returning false classifies the file as kForeign.
using Consumer = bool (*)(const uint8_t* plain, size_t len, void* ctx);

Status load(const uint8_t* key, vault::Kind kind, const char* path, Consumer consume, void* ctx);

// Seals plain (fresh random IV) and writes it, replacing the file contents.
bool save(const uint8_t* key, vault::Kind kind, const char* path, const uint8_t* plain, size_t plainLen);

// Tries primary, then backup. On kLoaded from backup, *usedBackup is set and
// the caller should re-save both slots to finish the repair. Foreign in
// either slot outranks corrupt/missing in the returned status.
Status loadAb(const uint8_t* key, vault::Kind kind, const char* primary, const char* backup, Consumer consume,
              void* ctx, bool* usedBackup);

} // namespace configfile
