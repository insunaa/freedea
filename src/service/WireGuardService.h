#pragma once

// Freedea — WireGuard client service (Phase 7.3). Brings a single-peer
// WireGuard tunnel up after Wi-Fi associates when settings.wireguard is
// enabled and complete, and tears it down on link loss, config change or
// disable. Off by default.
//
// The vendored library (lib/WireGuard) resolves the endpoint with blocking
// DNS and runs curve25519 on the caller's stack, so begin() runs on a
// dedicated short-lived task; teardown and state polling run on the main
// loop only, never while that task is inside begin() (single ownership per
// phase). While the tunnel is up the library makes it the lwIP default
// interface: ALL outbound traffic (AC, weather, portal replies) rides it, so
// the WG server must route/NAT whatever the device needs — see ATTRIBUTION.
//
// Failure policy (spike): a bring-up or handshake failure ends the attempt;
// retries back off 30 s .. 15 min for unattended recovery, and a link
// transition or config change resets the ladder. The peer handshake itself
// happens on the lwIP tcpip task; we only poll the up/down flag.

#include <Settings.h>
#include <WireGuard-ESP32.h>

#include <cstdint>

class WifiService;

class WireGuardService {
public:
  enum class Status : uint8_t {
    kDisabled,    // switch off, incomplete config, or Wi-Fi down (not trying)
    kConnecting,  // bring-up task running (DNS + netif add)
    kHandshaking, // tunnel up locally, waiting on the first key agreement
    kUp,          // peer handshake done; the tunnel carries traffic
    kFailed,      // last attempt failed (library logs the reason)
  };

  // Both referenced objects must outlive the service (main.cpp globals).
  void init(const settings::Settings& settings, const WifiService& wifi);

  // Main loop: consumes the bring-up result, polls the peer-up flag,
  // enforces the desired state and applies the retry backoff.
  void tick();

  Status status() const { return status_; }

  // True once after every status change (drives the Settings repaint).
  bool consumeStatusEdge() {
    const bool edge = statusEdge_;
    statusEdge_ = false;
    return edge;
  }

private:
  // getaddrinfo + curve25519 device init + handshake initiation run here;
  // high-water is logged at every attempt so the budget is verified, not
  // assumed.
  static constexpr uint32_t kTaskStackSize = 6144;
  static constexpr uint32_t kHandshakeTimeoutMs = 60000;

  static void taskTrampoline(void* arg);
  void run(); // task context: begin(), report, self-delete

  // True when enabled + configured + link is up.
  bool desired() const;
  // Main-loop only; must not run while the bring-up task is alive.
  void stopTunnel();
  void setStatus(Status next);

  const settings::Settings* settings_ = nullptr;
  const WifiService* wifi_ = nullptr;
  WireGuard wg_;

  bool taskRunning_ = false;
  volatile bool taskDone_ = false;
  // Task outcome: 1 begin ok, 2 begin failed, 3 cancelled (config/link went
  // away mid-flight and the task tore the tunnel down itself).
  volatile uint8_t taskOutcome_ = 0;
  bool started_ = false;
  bool linkWasUp_ = false;
  bool cancelRequested_ = false;

  // Bring-up config snapshot: the task reads only cfg_ (written at spawn,
  // untouched while it runs), never live settings. cfgLast_ tracks the live
  // config for change detection on the main loop; ownIpOctets_ is the parsed
  // tunnel IP of cfg_ (cfgValid_ also requires wireGuardConfigured()).
  settings::WireGuard cfg_ = {};
  settings::WireGuard cfgLast_ = {};
  bool cfgValid_ = false;
  uint8_t ownIpOctets_[4] = {};

  Status status_ = Status::kDisabled;
  bool statusEdge_ = false;
  uint8_t attemptShift_ = 0;
  uint32_t retryAtMs_ = 0;
  uint32_t handshakeDeadlineMs_ = 0;
};
