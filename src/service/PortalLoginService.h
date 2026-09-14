#pragma once

// Captive-portal auto-login service (Phase 7.2d). When the active profile's
// cached portal state says a login page waits and the link is up, the service
// raises one prompt per link session. On confirm a short-lived task GETs the
// connectivity-check URL (following redirects manually, capped) to reach the
// interception page, extracts its first <form> (lib/HtmlForm), fills it with
// defaults, submits it urlencoded (GET/POST honored), then re-probes: the
// exact empty 204 means joined, anything else did not work. HTTPS targets
// fail without attempting. The cached portal state never changes either way
// — a failed attempt just leaves the next connect prompt again.

#include <Settings.h>

#include <cstdint>

class WifiService;

class PortalLoginService {
public:
  // `settings` is the live SettingsStore copy (read-only here: only the
  // active profile's cached portal state gates the prompt). Both references
  // must outlive the service.
  void init(const settings::Settings& settings, const WifiService& wifi);

  // Main loop: true once per link session when the link is up and the active
  // profile caches a portal — the caller pushes the prompt screen. The
  // session stays prompted (Back dismisses; the next connect prompts again).
  bool consumePromptEdge();

  // Spawns the attempt task (prompt screen's Confirm). False when one is
  // already running or the task cannot be created.
  bool start();

  // An attempt is in flight (the prompt screen draws the busy state).
  bool running() const { return running_; }

  // True once after an attempt completes, carrying its verdict.
  bool consumeFinished(bool& joined);

  static constexpr uint32_t kTaskStackSize = 4096; // DNS + HTTPClient dominate

private:
  static void taskTrampoline(void* arg);
  void run();
  void finish(bool joined);

  const settings::Settings* settings_ = nullptr;
  const WifiService* wifi_ = nullptr;
  volatile bool running_ = false;
  volatile bool finishedEdge_ = false;
  volatile bool joined_ = false;
  bool linkWasUp_ = false;
  bool sessionPrompted_ = false;
};
