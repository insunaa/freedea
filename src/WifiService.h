#pragma once

// WiFi station service: connects to the active profile from settings.json
// (schema v3 multi-network store), reconnects with capped exponential
// backoff, and reports link transitions through a callback. State is sampled in tick() on the main loop rather than
// via Arduino WiFi event callbacks, keeping all state in one task; the core's
// WiFi.begin() is only an async kick (esp_wifi_connect), so polling status is
// cheap and race-free. The password is read on demand and never logged.

#include <Settings.h>

#include <cstddef>
#include <cstdint>

class WifiService {
public:
  enum class Link : uint8_t {
    kDisabled,     // no SSID configured; radio stays off
    kConnecting,   // a connect attempt is in flight
    kDisconnected, // between attempts (backoff) or after link loss
    kConnected,    // associated and holding an IPv4 lease
  };

  // `settings` must outlive this service (it is the global SettingsStore copy
  // in main.cpp). Starts the first connect attempt when a network is active.
  void init(const settings::Settings& settings);

  // Re-run the connection against the (possibly just changed) active network:
  // cancels any in-flight attempt, resets the backoff, and kicks a connect.
  // Called after the UI switches settings.activeNetwork.
  void restart();

  // Call from the main loop: samples link state, fires transition callbacks,
  // and retries with backoff after failures or link loss.
  void tick();

  Link link() const { return link_; }
  bool connected() const { return link_ == Link::kConnected; }

  // Last IPv4 lease as a dotted-quad string ("0.0.0.0" before the first one).
  void localIp(char* buf, size_t cap) const;

  // Invoked from tick() context on every link transition. One subscriber.
  using LinkCallback = void (*)(void* ctx, Link link);
  void setLinkCallback(LinkCallback callback, void* ctx) {
    callback_ = callback;
    callbackCtx_ = ctx;
  }

private:
  static constexpr uint32_t kFirstRetryDelayMs = 1000;
  static constexpr uint32_t kMaxRetryDelayMs = 30000;
  // A kick whose status never reaches WL_CONNECTED within this window is
  // abandoned (stalled association or DHCP) and rescheduled.
  static constexpr uint32_t kAttemptTimeoutMs = 15000;
  // Bounds kFirstRetryDelayMs << retryShift_ and already exceeds the cap.
  static constexpr uint8_t kMaxRetryShift = 5;
  static constexpr const char* kHostname = "freedea";

  void configureStack();
  void startAttempt();
  const settings::Network* activeNetwork() const;
  void scheduleRetry(uint32_t nowMs);
  void setLink(Link next);

  const settings::Settings* settings_ = nullptr;
  bool stackConfigured_ = false;
  Link link_ = Link::kDisabled;
  bool attemptInFlight_ = false;
  uint32_t attemptStartedMs_ = 0;
  uint32_t retryAtMs_ = 0;
  uint8_t retryShift_ = 0;
  uint8_t ip_[4] = {};
  LinkCallback callback_ = nullptr;
  void* callbackCtx_ = nullptr;
};
