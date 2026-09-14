#pragma once

// Firmware-side glue for the settings core (lib/Settings): mounts the SD
// card and keeps the live settings in an encrypted, CRC-checked binary store
// (/.freedea/settings.bin + .bak, key derived from the chip MAC). A plain
// settings.json on SD is treated as the import/recovery surface: it is only
// read when no valid encrypted store exists, and removed after a successful
// migration. The password is never logged.

#include <Settings.h>
#include <Vault.h>

class SettingsStore {
public:
  // Mounts the SD card, ensures /.freedea/, derives the store key and loads
  // settings.bin (repairing from .bak when one slot is corrupt). Falls back
  // to importing settings.json, then to defaults (persisted). All failures
  // are non-fatal: defaults stay in RAM and sdReady() reports the state.
  void init();

  // Call from the main loop; flushes pending changes kSaveDebounceMs after
  // the last markDirty(), retrying on write failure.
  void tick();

  // Schedules persistence of the current settings once changes settle.
  void markDirty();

  // Persists immediately (portal saves must not wait on the debounce).
  // Returns false if SD is unavailable or a write failed; on failure the
  // debounced retry path stays armed. Caller decides what to tell the user.
  bool saveNow();

  settings::Settings& settings() { return settings_; }
  const settings::Settings& settings() const { return settings_; }
  bool sdReady() const { return sdReady_; }

  // Recovery entry into a provisioning boot (step 3.5): write a one-shot
  // flag file; the caller reboots. The flag is consumed at boot by
  // consumeProvisionFlag(), so walking away without saving never bootloops.
  // Returns false when SD is unavailable or the write failed.
  bool requestProvision();
  // True if /.freedea/provision.flag exists at boot; always deletes it
  // (consumed whether or not provisioning then succeeds).
  bool consumeProvisionFlag();

private:
  static constexpr uint32_t kSaveDebounceMs = 2000;
  static constexpr const char* kDirPath = "/.freedea";
  static constexpr const char* kBinPath = "/.freedea/settings.bin";
  static constexpr const char* kBinBakPath = "/.freedea/settings.bak";
  static constexpr const char* kJsonPath = "/.freedea/settings.json";
  static constexpr const char* kProvisionFlagPath = "/.freedea/provision.flag";

  bool saveBoth();
  void importLegacyJson();

  settings::Settings settings_;
  uint8_t key_[vault::kKeyLen] = {};
  bool keyValid_ = false;
  bool dirty_ = false;
  uint32_t dirtyAtMs_ = 0;
  bool sdReady_ = false;
};
