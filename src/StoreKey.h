#pragma once

// Vault store-key derivation (3.4): sha256("freedea:vault:v1" || EFuse Wi-Fi
// MAC). The MAC never leaves the chip, so an SD card read elsewhere cannot
// derive it. Same key for every store file; the envelope's kind byte keeps
// the files from being interchangeable.

#include <Vault.h>

namespace storekey {

// Derives the shared store key. Returns false if esp_read_mac fails (the
// stores then stay RAM-only for the boot).
bool deriveKey(uint8_t out[vault::kKeyLen]);

} // namespace storekey
