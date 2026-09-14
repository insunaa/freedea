#pragma once

// Firmware side of the Midea device store (3.4): loads /.freedea/devices.bin
// (with .bak repair) at boot, imports the hand-editable device.json when no
// valid store exists, and persists changes. Names and lengths are logged —
// token/key material never is.

#include <Devices.h>
#include <Vault.h>

class DeviceStore {
public:
  // Call after SettingsStore::init() (SD already mounted). sdReady mirrors
  // the settings store's mount result; without SD the list stays empty.
  void init(bool sdReady);

  const devices::List& list() const { return list_; }
  bool sdReady() const { return sdReady_; }

  // Inserts or replaces by id. Returns the record index, or -1 when the
  // store is full (existing entries untouched). Caller persists via saveNow().
  int upsert(const devices::Device& device);

  // Removes the record with this id (remaining entries compact, vacated slot
  // zeroed). False when the id is not stored. Caller persists via saveNow().
  bool remove(uint64_t id);

  // Writes both slots immediately. False on SD/key failure.
  bool saveNow();

private:
  static constexpr const char* kDirPath = "/.freedea";
  static constexpr const char* kBinPath = "/.freedea/devices.bin";
  static constexpr const char* kBinBakPath = "/.freedea/devices.bak";
  static constexpr const char* kJsonPath = "/.freedea/device.json";

  bool saveBoth();
  void importJson();
  void logLoaded() const;

  devices::List list_;
  uint8_t key_[vault::kKeyLen] = {};
  bool keyValid_ = false;
  bool sdReady_ = false;
};
