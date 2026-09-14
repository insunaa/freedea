#pragma once

// Captive-portal probe service (Phase 7.2c). When the link comes up on the
// active profile while its cached portal state is still unknown, one
// short-lived task GETs the connectivity-check URL, classifies the reply
// (lib/CaptiveProbe) and caches the verdict in the profile; the task then
// deletes itself. Transport failures are not cached: the profile stays
// unknown and the next connect re-probes.
//
// The task exists only during a probe (4 KB stack: DNS + HTTPClient
// dominate, same budget as WeatherService) and is created at most once per
// link session. The verdict is applied on the main loop in consumeResult() —
// settings writes and the debounced persist stay single-task.

#include <Settings.h>

#include <cstdint>

class WifiService;

class CaptiveProbeService {
public:
  // Schedules the debounced settings save once a verdict has been cached
  // (wired to SettingsStore::markDirty from main.cpp). `settings` is the live
  // store copy and must outlive the service.
  using PersistFn = void (*)(void* ctx);
  void init(settings::Settings& settings, const WifiService& wifi, PersistFn persist, void* persistCtx);

  // Main loop: spawns the probe when the link is up, the active profile has
  // no cached portal state, and this link session has not tried yet.
  void tick();

  // Main loop: true once after a probe produced a verdict, which has been
  // written to the probed profile (dropped when that slot's SSID changed
  // while the probe was in flight) and scheduled for persistence. Callers
  // repaint the screens that show the cached state.
  bool consumeResult();

  static constexpr uint32_t kTaskStackSize = 4096; // DNS + HTTPClient dominate

private:
  static void taskTrampoline(void* arg);
  void run();

  settings::Settings* settings_ = nullptr;
  const WifiService* wifi_ = nullptr;
  PersistFn persist_ = nullptr;
  void* persistCtx_ = nullptr;

  volatile bool taskRunning_ = false; // spawn guard, cleared by the task last
  volatile bool resultReady_ = false; // verdict in pendingState_ for probeSlot_
  bool linkWasUp_ = false;
  bool sessionTried_ = false; // one attempt per link session, failures included
  uint8_t pendingState_ = settings::kPortalUnknown;
  uint8_t probeSlot_ = settings::kNoNetwork;
  char probeSsid_[settings::kSsidMaxLen + 1] = {}; // identity of the probed slot
};
