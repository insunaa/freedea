// Freedea — WireGuard client service (Phase 7.3). See WireGuardService.h.

#include "WireGuardService.h"

#include <Arduino.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/apps/sntp.h>
#include <lwip/ip4_addr.h>
#include <lwip/sockets.h>
#include <sys/time.h>
#ifdef FREEDDEA_DEBUG_WG
#include <lwip/netdb.h>
#endif

#include <cstdio>
#include <cstring>

#include "../WifiService.h"

namespace {

// FreeRTOS high-water marks on ESP-IDF are reported in bytes.
unsigned stackHwm(const char* taskName) {
  TaskHandle_t handle = xTaskGetHandle(taskName);
  return handle != nullptr ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(handle)) : 0;
}

// lwIP's DHCP→SNTP hand-off is push-only: dhcp_bind copies option-42 servers
// into the SNTP table only if sntp_servermode_dhcp(1) is already on, and no
// getter for the raw option exists in the ESP32 core. Enable the mode at
// service init — association plus DHCP take far longer than the microsecond
// gap between wifi.init() and us, so this wins against the first dhcp_bind;
// losing would merely fall through to the static server. sntp_servermode_dhcp
// asserts the lwIP core lock, hence the tcpip-thread dispatch.
esp_err_t enableDhcpNtp(void* ctx) {
  (void)ctx;
  sntp_servermode_dhcp(1);
  return ESP_OK;
}

// NTP server that dhcp_bind pushed into SNTP slot 0 (network-order IPv4 out).
// False when the network offers none. A previous lease's server lingers when
// a newer network offers none (dhcp_set_ntp_servers only clears slots when it
// delivers servers), so callers must still fall back on exchange failure.
// sntp_getserver() takes no core lock and only reads a static table; racing a
// dhcp_bind write can at worst yield a stale value, which the fallback
// absorbs.
bool ntpServerFromDhcp(uint32_t& serverAddr) {
  const ip_addr_t* srv = sntp_getserver(0);
  if (srv == nullptr || ip_addr_isany(srv)) {
    return false;
  }
  serverAddr = ip_2_ip4(srv)->addr;
  return true;
}

// One raw NTP exchange against the given network-order IPv4 server.
bool ntpExchange(uint32_t serverAddr) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return false;
  }
  timeval timeout{.tv_sec = 3, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(123);
  dst.sin_addr.s_addr = serverAddr;
  uint8_t pkt[48] = {0x1B}; // LI=0, VN=3, mode=3(client); rest zero per RFC 5905
  bool synced = false;
  if (sendto(fd, pkt, sizeof(pkt), 0, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) ==
      static_cast<ssize_t>(sizeof(pkt))) {
    uint8_t rx[48];
    const ssize_t n = recv(fd, rx, sizeof(rx), 0);
    // Reject leap-indicator "not synchronized" and stratum 0.
    if (n == static_cast<ssize_t>(sizeof(rx)) && ((rx[0] >> 6) & 3) != 3 && rx[1] != 0) {
      uint32_t secsBe = 0;
      memcpy(&secsBe, &rx[32], 4);                            // receive timestamp
      const uint32_t unixSecs = ntohl(secsBe) - 2208988800UL; // NTP epoch -> Unix
      const timeval tv{static_cast<time_t>(unixSecs), 0};
      settimeofday(&tv, nullptr);
      synced = true;
    }
  }
  close(fd);
  return synced;
}

// Sync the wall clock for the WG handshake, preferring the network's own NTP
// server (DHCP option 42) and falling back to Cloudflare anycast (fixed IPv4,
// so no DNS dependency). The device has no battery-backed clock, and the WG
// server silently discards initiations carrying the resulting ~1970 tai64n
// stamp (verified on device). Returns the server that answered (network-order
// IPv4) or 0. Runs before the tunnel binds the default route; best effort —
// failure only costs the handshake, which retries with backoff.
uint32_t syncClockForWg() {
  uint32_t dhcpServer = 0;
  if (ntpServerFromDhcp(dhcpServer) && ntpExchange(dhcpServer)) {
    return dhcpServer;
  }
  const uint32_t fallback = inet_addr("162.159.200.1"); // time.cloudflare.com
  if (fallback == dhcpServer) {
    return 0;
  }
  return ntpExchange(fallback) ? fallback : 0;
}

constexpr uint32_t kRetryBaseLocal = 30000;
constexpr uint32_t kRetryCapLocal = 900000;

// Exponential retry delay: 30 s doubling to the 15 min cap.
uint32_t retryDelayMs(uint8_t attemptShift) {
  uint32_t delayMs = kRetryBaseLocal << (attemptShift > 5 ? 5 : attemptShift);
  return delayMs > kRetryCapLocal ? kRetryCapLocal : delayMs;
}

// Dotted-quad into octets; rejects anything sscanf does not read as exactly
// four 0..255 groups.
bool parseOwnIp(const char* text, uint8_t octets[4]) {
  unsigned parts[4] = {0, 0, 0, 0};
  int consumed = 0;
  if (std::sscanf(text, "%3u.%3u.%3u.%3u%n", &parts[0], &parts[1], &parts[2], &parts[3], &consumed) != 4) {
    return false;
  }
  if (text[consumed] != '\0') {
    return false;
  }
  for (int i = 0; i < 4; i++) {
    if (parts[i] > 255) {
      return false;
    }
    octets[i] = static_cast<uint8_t>(parts[i]);
  }
  return true;
}

} // namespace

void WireGuardService::init(const settings::Settings& settings, const WifiService& wifi) {
  settings_ = &settings;
  wifi_ = &wifi;
  // Must land before this boot's first dhcp_bind for option-42 servers to
  // reach the SNTP table (see enableDhcpNtp).
  esp_netif_tcpip_exec(&enableDhcpNtp, nullptr);
}

bool WireGuardService::desired() const {
  const settings::WireGuard& wg = settings_->wireguard;
  return wg.enabled && settings::wireGuardConfigured(wg) && wifi_->connected();
}

void WireGuardService::setStatus(Status next) {
  if (next == status_) {
    return;
  }
  status_ = next;
  statusEdge_ = true;
}

void WireGuardService::stopTunnel() {
  if (wg_.is_initialized()) {
    wg_.end();
  }
  started_ = false;
  Serial.printf("[WG ] tunnel down, heap %lu\n", static_cast<unsigned long>(ESP.getFreeHeap()));
}

void WireGuardService::tick() {
  if (settings_ == nullptr) {
    return; // init never ran (provisioning boot): stay inert
  }
  const uint32_t now = millis();

  // 1. Consume a finished bring-up attempt (settings and lwIP stay
  //    main-loop-owned; the task only begins the tunnel and reports).
  if (taskDone_ && !taskRunning_) {
    taskDone_ = false;
    const uint8_t outcome = taskOutcome_;
    if (outcome == 1) {
      started_ = true;
      handshakeDeadlineMs_ = now + kHandshakeTimeoutMs;
      setStatus(Status::kHandshaking);
    } else if (outcome == 2) {
      attemptShift_++;
      retryAtMs_ = now + retryDelayMs(attemptShift_);
      setStatus(Status::kFailed);
    } else { // cancelled mid-flight by a config/link change
      setStatus(Status::kDisabled);
    }
  }

  // 2. Link transitions: a lost link kills the tunnel (its UDP path is gone)
  //    and resets the retry ladder — reconnection is a fresh chance.
  const bool linkUp = wifi_->connected();
  if (linkWasUp_ && !linkUp) {
    if (taskRunning_) {
      cancelRequested_ = true;
    } else if (started_) {
      stopTunnel();
    }
    attemptShift_ = 0;
    retryAtMs_ = 0;
    if (!taskRunning_) {
      setStatus(Status::kDisabled);
    }
  }
  linkWasUp_ = linkUp;

  // 3. Config changes (portal save): tear down and re-evaluate. The live
  //    copy is diffed against cfgLast_; the running snapshot cfg_ stays
  //    frozen while the bring-up task reads it.
  if (std::memcmp(&cfgLast_, &settings_->wireguard, sizeof(cfgLast_)) != 0) {
    cfgLast_ = settings_->wireguard;
    if (taskRunning_) {
      cancelRequested_ = true;
    } else if (started_) {
      stopTunnel();
      setStatus(Status::kDisabled);
    }
    attemptShift_ = 0;
    retryAtMs_ = 0;
  }

  // 4. Tunnel is up locally: watch the peer handshake flag (written by the
  //    lwIP tcpip task; a benign single-word read). Stalled handshakes get
  //    the same treatment as failures so the backoff ladder applies.
  if (started_) {
    const bool up = wg_.is_up();
    if (up) {
      if (status_ != Status::kUp) {
        setStatus(Status::kUp);
        Serial.printf("[WG ] peer up, heap %lu, tcpip stack hwm %u B\n", static_cast<unsigned long>(ESP.getFreeHeap()),
                      stackHwm("tcpip"));
      }
    } else {
      if (status_ == Status::kUp) {
        setStatus(Status::kHandshaking); // keys expired / peer gone: re-handshake
        handshakeDeadlineMs_ = now + kHandshakeTimeoutMs;
        Serial.println("[WG ] peer lost, re-handshaking");
      }
      if (status_ == Status::kHandshaking && static_cast<int32_t>(now - handshakeDeadlineMs_) >= 0) {
        Serial.println("[WG ] handshake timeout");
        stopTunnel();
        attemptShift_++;
        retryAtMs_ = now + retryDelayMs(attemptShift_);
        setStatus(Status::kFailed);
      }
    }
  }

  // 5. Spawn a bring-up when desired, not already up/running, and past the
  //    backoff. One task at a time; failures never spin.
  if (!taskRunning_ && !started_ && desired() && static_cast<int32_t>(now - retryAtMs_) >= 0) {
    cfg_ = cfgLast_;
    cfgValid_ = parseOwnIp(cfg_.ownIp, ownIpOctets_);
    if (!cfgValid_) {
      // A malformed tunnel IP would fail every attempt identically; surface
      // it as a persistent failure without spawning.
      setStatus(Status::kFailed);
      Serial.println("[WG ] invalid tunnel IP in settings");
      retryAtMs_ = now + kRetryCapLocal;
      return;
    }
    cancelRequested_ = false;
    taskRunning_ = true;
    setStatus(Status::kConnecting);
    Serial.printf("[WG ] bring-up: %s:%u keepalive %u s, heap %lu\n", cfg_.endpoint, static_cast<unsigned>(cfg_.port),
                  static_cast<unsigned>(cfg_.keepalive), static_cast<unsigned long>(ESP.getFreeHeap()));
    if (xTaskCreate(&WireGuardService::taskTrampoline, "wgap", kTaskStackSize, this, 1, nullptr) != pdPASS) {
      taskRunning_ = false;
      setStatus(Status::kFailed);
      retryAtMs_ = now + kRetryBaseLocal;
      Serial.println("[WG ] bring-up task not created (out of memory); will retry");
    }
  }
}

void WireGuardService::taskTrampoline(void* arg) {
  static_cast<WireGuardService*>(arg)->run();
}

void WireGuardService::run() {
  const uint32_t heapBefore = ESP.getFreeHeap();
  const uint32_t t0 = millis();

#ifdef FREEDDEA_DEBUG_WG
  // Diagnostic: print where handshakes actually go before begin() resolves it
  // itself. Distinguishes a wrong DNS answer (hotspot/carrier DNS can differ
  // from the home network's) from outbound UDP being blocked. Debug-only; the
  // flag lives solely in env:x4.
  {
    addrinfo* res = nullptr;
    if (getaddrinfo(cfg_.endpoint, nullptr, nullptr, &res) == 0 && res != nullptr && res->ai_addr != nullptr) {
      const auto* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
      const uint8_t* b = reinterpret_cast<const uint8_t*>(&sin->sin_addr.s_addr);
      Serial.printf("[WG ] dns: %s -> %u.%u.%u.%u\n", cfg_.endpoint, b[0], b[1], b[2], b[3]);
    } else {
      Serial.printf("[WG ] dns: %s lookup failed\n", cfg_.endpoint);
    }
    if (res != nullptr) {
      freeaddrinfo(res);
    }
  }
#endif

  // Required, not best-effort courtesy: the WG server silently discards
  // handshakes whose ~1970 unsynced timestamps it deems stale (see
  // syncClockForWg). Proceed anyway on failure — a lenient server still
  // handshakes, and failures retry with backoff.
  const uint32_t ntpServer = syncClockForWg();
  if (ntpServer != 0) {
    Serial.printf("[WG ] ntp: synced via %u.%u.%u.%u, epoch now %ld\n", ntpServer & 0xff, (ntpServer >> 8) & 0xff,
                  (ntpServer >> 16) & 0xff, (ntpServer >> 24) & 0xff, static_cast<long>(time(nullptr)));
  } else {
    Serial.println("[WG ] ntp: FAILED (dhcp + fallback)");
  }

  const IPAddress tunnelIp(ownIpOctets_[0], ownIpOctets_[1], ownIpOctets_[2], ownIpOctets_[3]);
  // begin() blocks on DNS (up to ~10 s of retries) and runs curve25519 for
  // the device keypair plus the handshake initiation on this stack.
  const bool ok = wg_.begin(tunnelIp, cfg_.ownPrivateKey, cfg_.endpoint, cfg_.peerPublicKey, cfg_.port, cfg_.keepalive);
  const uint32_t elapsedMs = millis() - t0;
  Serial.printf("[WG ] begin %s in %lu ms, heap %lu -> %lu, task stack hwm %u B, tcpip stack hwm %u B\n",
                ok ? "ok" : "FAILED", static_cast<unsigned long>(elapsedMs), static_cast<unsigned long>(heapBefore),
                static_cast<unsigned long>(ESP.getFreeHeap()), stackHwm("wgap"), stackHwm("tcpip"));

  taskOutcome_ = 0;
  if (ok) {
    if (cancelRequested_ || !desired()) {
      // Config or link went away while resolving: close what we opened
      // before the main loop takes tunnel ownership back.
      wg_.end();
      taskOutcome_ = 3;
    } else {
      taskOutcome_ = 1;
    }
  } else {
    taskOutcome_ = 2;
  }
  taskDone_ = true;
  taskRunning_ = false;
  vTaskDelete(nullptr);
}
