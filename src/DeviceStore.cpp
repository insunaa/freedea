#include "DeviceStore.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include "ConfigFile.h"
#include "StoreKey.h"

// Import-file scratch: four fully-populated records (256-char tokens, name,
// key) fit comfortably; larger files are rejected rather than streamed.
static char sJsonBuf[2048];

namespace {

bool consumeDevices(const uint8_t* plain, size_t len, void* ctx) {
  return devices::deserializeBin(plain, len, *static_cast<devices::List*>(ctx));
}

} // namespace

void DeviceStore::init(bool sdReady) {
  sdReady_ = sdReady;
  if (!sdReady_) {
    Serial.println("[DEV] no SD card, device list empty");
    return;
  }
  if (!storekey::deriveKey(key_)) {
    Serial.println("[DEV] MAC unavailable, device list stays in RAM");
    return;
  }
  keyValid_ = true;

  bool usedBackup = false;
  const configfile::Status status =
      configfile::loadAb(key_, vault::Kind::kDevices, kBinPath, kBinBakPath, consumeDevices, &list_, &usedBackup);
  switch (status) {
    case configfile::Status::kLoaded:
      if (usedBackup) {
        Serial.println("[DEV] devices.bin unreadable, restored from backup");
        saveBoth();
      }
      logLoaded();
      return;
    case configfile::Status::kForeign:
      Serial.println("[DEV] devices store not from this device, ignoring it (no import, no repair)");
      return;
    default:
      // Numeric status pins missing vs corrupt vs error for the field logs.
      Serial.printf("[DEV] devices.bin status=%d\n", static_cast<int>(status));
      break; // Missing/Corrupt/Error: try the JSON import surface below.
  }

  if (SdMan.exists(kJsonPath)) {
    importJson();
    return;
  }
  if (status != configfile::Status::kMissing && status != configfile::Status::kCorrupt) {
    Serial.println("[DEV] device store load failed");
    return;
  }
  Serial.println("[DEV] no device.json found, device list empty");
}

bool DeviceStore::saveNow() {
  if (!sdReady_ || !keyValid_) return false;
  return saveBoth();
}

int DeviceStore::upsert(const devices::Device& device) {
  for (uint8_t i = 0; i < list_.count; ++i) {
    if (list_.devices[i].id == device.id) {
      list_.devices[i] = device;
      return i;
    }
  }
  if (list_.count >= devices::kMaxDevices) return -1;
  list_.devices[list_.count] = device;
  ++list_.count;
  return list_.count - 1;
}

bool DeviceStore::remove(uint64_t id) {
  for (uint8_t i = 0; i < list_.count; ++i) {
    if (list_.devices[i].id != id) continue;
    for (uint8_t j = i; j + 1 < list_.count; ++j) {
      list_.devices[j] = list_.devices[j + 1];
    }
    --list_.count;
    list_.devices[list_.count] = devices::Device{};
    return true;
  }
  return false;
}

bool DeviceStore::saveBoth() {
  // Static: 836 B payload scratch, main-loop only (stack budget stays small).
  static uint8_t sBin[devices::kBinPayloadSize];
  devices::serializeBin(list_, sBin);
  const bool a = configfile::save(key_, vault::Kind::kDevices, kBinPath, sBin, sizeof(sBin));
  const bool b = configfile::save(key_, vault::Kind::kDevices, kBinBakPath, sBin, sizeof(sBin));
  if (!a || !b) Serial.println("[DEV] device store write failed");
  return a && b;
}

void DeviceStore::importJson() {
  const size_t len = SdMan.readFileToBuffer(kJsonPath, sJsonBuf, sizeof(sJsonBuf));
  if (len == 0) {
    Serial.println("[DEV] failed to read device.json");
    return;
  }
  const devices::ImportError err = devices::parseImportJson(sJsonBuf, list_);
  if (err != devices::ImportError::kNone) {
    // The file stays for inspection; only the cause is logged, never content.
    Serial.printf("[DEV] device.json rejected (error %d), list stays empty\n", static_cast<int>(err));
    return;
  }
  if (!saveBoth()) {
    Serial.println("[DEV] imported device.json for this boot only (SD write failed)");
    logLoaded();
    return;
  }
  if (SdMan.remove(kJsonPath)) {
    Serial.println("[DEV] imported device.json into encrypted store, plaintext file removed");
  } else {
    Serial.println("[DEV] imported and saved, but could not remove device.json");
  }
  logLoaded();
}

void DeviceStore::logLoaded() const {
  Serial.printf("[DEV] %u device(s)\n", static_cast<unsigned>(list_.count));
  for (uint8_t i = 0; i < list_.count; ++i) {
    const devices::Device& d = list_.devices[i];
    // id + name only: token/key never reach the log.
    Serial.printf("[DEV]  #%u id=%llu name=%s ver=%u token=%uB ip=%u.%u.%u.%u\n", static_cast<unsigned>(i),
                  static_cast<unsigned long long>(d.id), d.name, static_cast<unsigned>(d.version),
                  static_cast<unsigned>(d.tokenLen), static_cast<unsigned>(d.ip[0]), static_cast<unsigned>(d.ip[1]),
                  static_cast<unsigned>(d.ip[2]), static_cast<unsigned>(d.ip[3]));
  }
}
