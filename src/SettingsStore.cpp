#include "SettingsStore.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include "ConfigFile.h"
#include "StoreKey.h"

namespace {

// JSON import scratch (legacy/import surface only); the binary path streams
// through ConfigFile's shared statics.
char sJsonBuf[settings::kSerializedMaxSize];

bool consumeSettings(const uint8_t* plain, size_t len, void* ctx) {
  return settings::deserializeBin(plain, len, static_cast<SettingsStore*>(ctx)->settings());
}

} // namespace

void SettingsStore::init() {
  if (!SdMan.begin()) {
    Serial.println("[CFG] SD card unavailable, using default settings");
    return;
  }
  sdReady_ = true;

  if (!SdMan.ensureDirectoryExists(kDirPath)) {
    Serial.println("[CFG] could not create /.freedea, settings will not persist");
  }

  if (!storekey::deriveKey(key_)) {
    Serial.println("[CFG] MAC unavailable, settings stay in RAM");
    return;
  }
  keyValid_ = true;

  bool usedBackup = false;
  const configfile::Status status =
      configfile::loadAb(key_, vault::Kind::kSettings, kBinPath, kBinBakPath, consumeSettings, this, &usedBackup);
  switch (status) {
    case configfile::Status::kLoaded:
      if (usedBackup) {
        Serial.println("[CFG] settings.bin unreadable, restored from backup");
        saveBoth();
      }
      return;
    case configfile::Status::kForeign:
      // Another device's card (key mismatch) or a format this firmware cannot
      // read: keep defaults, touch nothing.
      Serial.println("[CFG] settings store not from this device, using defaults (files untouched)");
      return;
    default:
      break; // Missing/Corrupt/Error: fall through to the JSON import surface.
  }

  if (SdMan.exists(kJsonPath)) {
    importLegacyJson();
    return;
  }

  // Fresh card: write the defaults so later boots are stable.
  if (saveNow()) {
    Serial.println("[CFG] created default settings store");
  }
}

void SettingsStore::tick() {
  if (dirty_ && millis() - dirtyAtMs_ >= kSaveDebounceMs) {
    if (saveNow()) {
      dirty_ = false;
    } else {
      dirtyAtMs_ = millis(); // Write failed; retry after another debounce window.
    }
  }
}

void SettingsStore::markDirty() {
  dirty_ = true;
  dirtyAtMs_ = millis();
}

bool SettingsStore::saveNow() {
  if (!sdReady_ || !keyValid_) {
    Serial.println("[CFG] settings not persisted (no SD or no store key)");
    return false;
  }
  if (!saveBoth()) {
    Serial.println("[CFG] immediate settings save failed, retry deferred");
    markDirty();
    return false;
  }
  dirty_ = false;
  return true;
}

bool SettingsStore::requestProvision() {
  if (!sdReady_) {
    Serial.println("[CFG] provision requested without SD, ignored");
    return false;
  }
  // Content is irrelevant; existence is the flag. A lost write just means the
  // next boot is normal — the safe direction.
  if (!SdMan.writeFile(kProvisionFlagPath, String("1"))) {
    Serial.println("[CFG] could not write provision.flag");
    return false;
  }
  Serial.println("[CFG] provision.flag written, rebooting into provisioning");
  return true;
}

bool SettingsStore::consumeProvisionFlag() {
  if (!sdReady_ || !SdMan.exists(kProvisionFlagPath)) {
    return false;
  }
  // Consumed even if the delete fails: a card that cannot delete would
  // otherwise bootloop provisioning forever, and a broken card is not
  // fixable by reprovisioning.
  if (!SdMan.remove(kProvisionFlagPath)) {
    Serial.println("[CFG] provision.flag present but not deletable, provisioning this boot only");
  } else {
    Serial.println("[CFG] provision.flag consumed, provisioning boot");
  }
  return true;
}

bool SettingsStore::saveBoth() {
  // File-scope scratch: the v3 payload exceeds the stack budget. Both call
  // paths (tick's debounced save and the portal's saveNow) run on the main
  // loop, so no locking is needed.
  static uint8_t bin[settings::kBinPayloadSize];
  settings::serializeBin(settings_, bin);
  const bool a = configfile::save(key_, vault::Kind::kSettings, kBinPath, bin, sizeof(bin));
  const bool b = configfile::save(key_, vault::Kind::kSettings, kBinBakPath, bin, sizeof(bin));
  return a && b;
}

void SettingsStore::importLegacyJson() {
  const size_t len = SdMan.readFileToBuffer(kJsonPath, sJsonBuf, sizeof(sJsonBuf));
  if (len == 0 || !settings::deserialize(sJsonBuf, settings_)) {
    // Keep the file for inspection (and so it can be fixed and retried).
    Serial.println("[CFG] malformed settings.json, using defaults (file left unchanged)");
    return;
  }
  if (!saveBoth()) {
    Serial.println("[CFG] imported settings.json for this boot only (SD write failed)");
    return;
  }
  // The plaintext copy is redundant now — remove it so credentials do not
  // linger on the card unencrypted. Recovery re-import: delete settings.bin
  // and settings.bak, restore the JSON, reboot.
  if (SdMan.remove(kJsonPath)) {
    Serial.println("[CFG] migrated settings.json to encrypted store, plaintext file removed");
  } else {
    Serial.println("[CFG] migrated settings, but could not remove settings.json");
  }
  const settings::Network* net = settings::activeNetwork(settings_);
  Serial.printf("[CFG] loaded ssid=%s, password=%s\n", net ? net->ssid : "<unset>",
                net && net->password[0] ? "(set)" : "<unset>");
}
