#include "WifiService.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cstdio>

namespace {

// Arduino event task is never entered: everything runs on the main loop.
void formatIp(const uint8_t ip[4], char* buf, size_t cap) {
  snprintf(buf, cap, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

} // namespace

void WifiService::init(const settings::Settings& settings) {
  settings_ = &settings;
  if (!settings::anyNetwork(*settings_)) {
    link_ = Link::kDisabled;
    Serial.println("[WIFI] no network configured, WiFi stays off");
    return;
  }

  configureStack();

  const uint32_t heapBefore = ESP.getFreeHeap();
  startAttempt();
  Serial.printf("[WIFI] wifi stack heap: %u -> %u\n", heapBefore, ESP.getFreeHeap());
}

void WifiService::tick() {
  if (link_ == Link::kDisabled) {
    return;
  }

  const wl_status_t status = WiFi.status();
  const uint32_t now = millis();

  if (status == WL_CONNECTED) {
    if (link_ != Link::kConnected) {
      attemptInFlight_ = false;
      retryShift_ = 0;
      const IPAddress ip = WiFi.localIP();
      for (size_t i = 0; i < 4; i++) {
        ip_[i] = static_cast<uint8_t>(ip[i]);
      }
      char ipStr[16];
      formatIp(ip_, ipStr, sizeof(ipStr));
      const settings::Network* net = activeNetwork();
      Serial.printf("[WIFI] connected to \"%s\", ip %s, rssi %d dBm, heap %u\n", net ? net->ssid : "?", ipStr,
                    static_cast<int>(WiFi.RSSI()), ESP.getFreeHeap());
      setLink(Link::kConnected);
    }
    return;
  }

  if (attemptInFlight_) {
    // Still inside the attempt window for an association/DHCP in progress.
    if (status != WL_CONNECT_FAILED && status != WL_NO_SSID_AVAIL && now - attemptStartedMs_ < kAttemptTimeoutMs) {
      return;
    }
    attemptInFlight_ = false;
    if (now - attemptStartedMs_ >= kAttemptTimeoutMs && status != WL_CONNECT_FAILED && status != WL_NO_SSID_AVAIL) {
      // Cancels the pending esp_wifi_connect so the next kick starts fresh.
      WiFi.disconnect(false);
      Serial.println("[WIFI] connect attempt timed out");
    } else {
      Serial.printf("[WIFI] connect attempt failed (status %u)\n", static_cast<unsigned>(status));
    }
    setLink(Link::kDisconnected);
    scheduleRetry(now);
  } else if (link_ == Link::kConnected) {
    // Deasserted between ticks: lost the association (or the lease's link).
    Serial.println("[WIFI] link lost");
    setLink(Link::kDisconnected);
    scheduleRetry(now);
  }

  if (!attemptInFlight_ && link_ != Link::kConnected && static_cast<int32_t>(now - retryAtMs_) >= 0) {
    startAttempt();
  }
}

void WifiService::localIp(char* buf, size_t cap) const {
  formatIp(ip_, buf, cap);
}

const settings::Network* WifiService::activeNetwork() const {
  return settings_ ? settings::activeNetwork(*settings_) : nullptr;
}

void WifiService::restart() {
  if (!settings_) return;
  if (!settings::anyNetwork(*settings_)) {
    // Nothing to connect to: leave a disabled service disabled (an active
    // link, if any, drops through tick() like any other loss).
    return;
  }
  // Cancel any in-flight attempt (also drops the current association) and
  // start fresh against the new active profile.
  WiFi.disconnect(false);
  attemptInFlight_ = false;
  retryShift_ = 0;
  configureStack();
  startAttempt();
}

void WifiService::configureStack() {
  if (stackConfigured_) {
    return;
  }
  stackConfigured_ = true;
  // settings.json is the credential store of record; keep the core from
  // mirroring them into NVS (duplicated secret, extra flash wear).
  WiFi.persistent(false);
  WiFi.setHostname(kHostname);
  WiFi.mode(WIFI_STA);
  // 5.4: modem power save ON (WIFI_PS_MIN_MODEM). With PS off, a connected
  // radio idles at ~85 mA — the battery's largest steady consumer. The AC path
  // (TCP, 5 s GetState polling) absorbs the beacon-interval (≤ ~0.3 s) wake
  // delay easily; the old "latency-sensitive UDP" rationale predates the TCP
  // transport.
  WiFi.setSleep(true);
}

void WifiService::startAttempt() {
  const settings::Network* net = activeNetwork();
  if (!net) {
    // Active profile vanished from under the service (should not happen:
    // callers only switch between filled slots). Stay disabled.
    Serial.println("[WIFI] no active network, WiFi stays off");
    setLink(Link::kDisabled);
    return;
  }
  attemptStartedMs_ = millis();
  attemptInFlight_ = true;
  setLink(Link::kConnecting);
  Serial.printf("[WIFI] connecting to \"%s\"\n", net->ssid);
  if (WiFi.begin(net->ssid, net->password) == WL_CONNECT_FAILED) {
    // Synchronous rejection (driver or config fault); the same backoff as an
    // async failure applies.
    attemptInFlight_ = false;
    Serial.println("[WIFI] WiFi.begin failed");
    setLink(Link::kDisconnected);
    scheduleRetry(millis());
  }
}

void WifiService::scheduleRetry(uint32_t nowMs) {
  uint32_t delayMs = kFirstRetryDelayMs << retryShift_;
  if (delayMs > kMaxRetryDelayMs) {
    delayMs = kMaxRetryDelayMs;
  }
  if (retryShift_ < kMaxRetryShift) {
    retryShift_++;
  }
  retryAtMs_ = nowMs + delayMs;
}

void WifiService::setLink(Link next) {
  if (next == link_) {
    return;
  }
  link_ = next;
  if (callback_) {
    callback_(callbackCtx_, link_);
  }
}
