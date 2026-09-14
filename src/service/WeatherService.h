#pragma once

// Weather service (Phase 6.2): a low-priority task that fetches current
// conditions plus a 12 h hourly forecast from Open-Meteo over plain HTTP
// every 30 minutes while the device runs (one fetch as soon as STA is up,
// plus manual refresh). Plain HTTP is deliberate: no key material to ship,
// the data is non-secret, and a TLS handshake would peak ~40-50 KB DRAM
// against the ~110 KB headroom (guest-AP egress device-verified 2026-09-07).
//
// The response body is treated as untrusted: only a 200 with a parseable
// Open-Meteo body updates the snapshot; redirects, captive portals, error
// pages, oversized or truncated bodies keep the previous snapshot and raise
// a one-shot error message for the toast. Failed attempts back off to the
// next 30-minute tick (no retry storm).
//
// Config lives in settings.weather (portal form); the task re-reads it each
// wake, so changes apply hot. Cross-task reads follow the WifiService
// convention (aligned scalar reads; the task never reads the display name).

#include <Settings.h>
#include <WeatherParser.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdint>

class WifiService;

class WeatherService {
public:
  struct Snapshot {
    weather::Snapshot data;
    uint32_t atMs = 0; // millis() of the successful fetch
  };

  // Starts the fetch task on a normal boot only (never during a provisioning
  // boot). Both references must outlive the service (main.cpp globals).
  // Returns false only if the mutex/task allocation failed; the service then
  // stays inert and the Dashboard panel never appears.
  bool init(const settings::Settings& settings, const WifiService& wifi);

  // Latest published snapshot; false while none exists (never fetched).
  bool snapshot(Snapshot& out) const;
  // Age of the last successful fetch; 0xFFFFFFFF when there is none.
  uint32_t lastFetchAgeMs() const;
  // True once per published snapshot; consumed by the main loop, which
  // repaints the Dashboard only while it is shown.
  bool consumeUpdate();
  // Skip the schedule wait (Settings -> Weather).
  void requestRefresh() { refreshRequested_ = true; }
  // One-shot error message (latest wins), drained by the main loop into a
  // toast.
  bool consumeError(char* buf, size_t cap);

  static constexpr uint32_t kTaskStackSize = 4096; // DNS + HTTPClient dominate

private:
  static constexpr uint32_t kFetchIntervalMs = 30u * 60u * 1000u;
  static constexpr uint32_t kWakeMs = 1000;
  // Open-Meteo replies ~1 KB for the shipped field set; anything larger is
  // not our forecast and is discarded whole (never parsed truncated).
  static constexpr size_t kBodyCapacity = 2048;

  static void taskTrampoline(void* arg);
  void run();
  void fetchOnce();
  void reportError(const char* msg);

  mutable SemaphoreHandle_t mutex_ = nullptr;
  const settings::Settings* settings_ = nullptr;
  const WifiService* wifi_ = nullptr;
  Snapshot snapshot_;
  bool hasSnapshot_ = false;
  bool updateEdge_ = false;
  bool everAttempted_ = false;
  uint32_t lastAttemptMs_ = 0;
  volatile bool refreshRequested_ = false;
  char errorMsg_[40] = {};
  bool errorEdge_ = false;
};
