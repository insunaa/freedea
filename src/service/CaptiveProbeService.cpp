// Freedea — captive-portal probe task (Phase 7.2c). See CaptiveProbeService.h.

#include "CaptiveProbeService.h"

#include <Arduino.h>
#include <CaptiveProbe.h>
#include <HTTPClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "../WifiService.h"

static_assert(captive::kOpen == settings::kPortalOpen && captive::kPortal == settings::kPortalFound,
              "probe verdicts must store straight into the profile cache");

namespace {

constexpr uint32_t kTimeoutMs = 8000;
// Only "empty vs. some bytes" decides the verdict, so a small cap bounds the
// download of an interception page without losing that bit (an overflow is
// by construction non-empty).
constexpr size_t kBodyCapacity = 128;

// Write-only capped Stream sink (same pattern as WeatherService's): HTTPClient's
// writeToStream de-chunks the transfer; everything past the cap is dropped.
class BodySink : public Stream {
public:
  BodySink(char* buf, size_t cap) : buf_(buf), cap_(cap) {}

  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    const size_t space = cap_ - len_;
    const size_t n = size <= space ? size : space;
    if (n > 0) {
      std::memcpy(buf_ + len_, data, n);
      len_ += n;
    }
    return size; // claim it all so writeToStream drains the response cleanly
  }

  // Unused read side of Stream.
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  size_t length() const { return len_; }

private:
  char* buf_;
  const size_t cap_;
  size_t len_ = 0;
};

} // namespace

void CaptiveProbeService::init(settings::Settings& settings, const WifiService& wifi, PersistFn persist,
                               void* persistCtx) {
  settings_ = &settings;
  wifi_ = &wifi;
  persist_ = persist;
  persistCtx_ = persistCtx;
}

void CaptiveProbeService::tick() {
  if (settings_ == nullptr || wifi_ == nullptr) {
    return; // init never ran (provisioning boot): stay inert
  }
  const bool linkUp = wifi_->connected();
  if (!linkUp) {
    linkWasUp_ = false;
    sessionTried_ = false;
    return;
  }
  if (!linkWasUp_) {
    linkWasUp_ = true;
    sessionTried_ = false; // fresh link session: probing allowed again
  }
  if (taskRunning_ || sessionTried_) {
    return;
  }

  const uint8_t slot = settings_->activeNetwork;
  if (slot >= settings::kMaxNetworks) {
    return;
  }
  const settings::Network& net = settings_->networks[slot];
  if (net.ssid[0] == '\0' || net.portalState != settings::kPortalUnknown) {
    return;
  }

  // One attempt per link session (failures included): a failed probe is
  // re-tried on the next connect, never hammered while the link stays up.
  sessionTried_ = true;
  probeSlot_ = slot;
  std::memcpy(probeSsid_, net.ssid, sizeof(probeSsid_));
  taskRunning_ = true;
  if (xTaskCreate(&CaptiveProbeService::taskTrampoline, "captive", kTaskStackSize, this, 1, nullptr) != pdPASS) {
    taskRunning_ = false;
    Serial.println("[CPB] probe task not created (out of memory); retried on next connect");
    return;
  }
  Serial.printf("[CPB] probing slot %u, heap %lu\n", static_cast<unsigned>(slot),
                static_cast<unsigned long>(ESP.getFreeHeap()));
}

bool CaptiveProbeService::consumeResult() {
  if (!resultReady_) {
    return false;
  }
  resultReady_ = false;
  if (probeSlot_ >= settings::kMaxNetworks || std::strcmp(settings_->networks[probeSlot_].ssid, probeSsid_) != 0) {
    // The probed network was edited away while the probe was in flight: the
    // verdict belongs to a network that no longer exists in that slot.
    Serial.println("[CPB] probed network changed mid-flight; verdict dropped");
    return false;
  }
  settings_->networks[probeSlot_].portalState = pendingState_;
  if (persist_ != nullptr) {
    persist_(persistCtx_);
  }
  return true;
}

void CaptiveProbeService::taskTrampoline(void* arg) {
  static_cast<CaptiveProbeService*>(arg)->run();
}

void CaptiveProbeService::run() {
  // Static like the other task scratch: keeps the 4 KB stack budget intact.
  static char sBody[kBodyCapacity];

  HTTPClient http;
  http.begin(captive::kProbeUrl);
  http.setConnectTimeout(kTimeoutMs);
  http.setTimeout(kTimeoutMs);
  // Following redirects stays disabled (the framework default): a 3xx is a
  // verdict of its own — usually the portal redirect — not something to
  // chase.
  const int status = http.GET();
  size_t bodyBytes = 0;
  if (status > 0) {
    BodySink sink(sBody, kBodyCapacity);
    http.writeToStream(&sink);
    bodyBytes = sink.length();
  }
  http.end();

  if (status <= 0) {
    // Connect/DNS failure: not evidence of anything. The profile stays
    // unknown and the next link-up re-probes.
    Serial.printf("[CPB] probe failed (status=%d); not cached\n", status);
  } else {
    pendingState_ = captive::classify(status, bodyBytes);
    resultReady_ = true;
    Serial.printf("[CPB] probe status=%d body=%u -> %s, heap %lu\n", status, static_cast<unsigned>(bodyBytes),
                  pendingState_ == captive::kOpen ? "open" : "portal", static_cast<unsigned long>(ESP.getFreeHeap()));
  }
  taskRunning_ = false;
  vTaskDelete(nullptr);
}
