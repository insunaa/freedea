// Freedea — captive-portal auto-login task (Phase 7.2d). See
// PortalLoginService.h. Every step is capped: redirects, timeouts, body
// size; any failure ends in the caller-visible "did not join" verdict.

#include "PortalLoginService.h"

#include <Arduino.h>
#include <CaptiveProbe.h>
#include <HTTPClient.h>
#include <HtmlForm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "../WifiService.h"

namespace {

constexpr uint32_t kTimeoutMs = 8000;
constexpr uint8_t kMaxRedirects = 5;
// Interception-page cap: a portal's first <form> sits near the top; an
// oversized body is rejected whole (never parsed truncated).
constexpr size_t kPageCapacity = 8192;
constexpr size_t kReprobeBodyCap = 64; // only "empty vs some bytes" matters

// Static scratch (task-stack budget): the page body doubles as the re-probe
// sink after the form was extracted from it.
char sPage[kPageCapacity + 1];
char sUrl[htmlform::kMaxUrl];
char sSubmitUrl[htmlform::kMaxUrl];
char sBody[htmlform::kMaxBody];
htmlform::Form sForm;

// Write-only capped Stream sink (same pattern as WeatherService's).
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
    if (size > space) overflow_ = true;
    return size; // claim it all so writeToStream drains the response cleanly
  }

  // Unused read side of Stream.
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  size_t length() const { return len_; }
  bool overflow() const { return overflow_; }

private:
  char* buf_;
  const size_t cap_;
  size_t len_ = 0;
  bool overflow_ = false;
};

enum class FetchResult : uint8_t {
  kAlreadyOpen,    // exact empty 204: no interception after all
  kHavePage,       // sPage holds the interception page (sPageLen bytes)
  kTransportFailed // connect/DNS failure, redirect storm exhausted, oversized, or https hop
};

size_t sPageLen = 0;

// GETs sUrl following redirects manually (capped); on kHavePage, sUrl is
// the URL that served the page and sPage its body. A redirect off http://
// counts as transport failure (the plan: https fails without attempting).
FetchResult fetchInterceptionPage() {
  for (uint8_t hop = 0; hop <= kMaxRedirects; ++hop) {
    HTTPClient http;
    http.begin(sUrl);
    http.setConnectTimeout(kTimeoutMs);
    http.setTimeout(kTimeoutMs);
    const int status = http.GET();
    if (status <= 0) {
      Serial.printf("[PLG] fetch failed (status=%d)\n", status);
      http.end();
      return FetchResult::kTransportFailed;
    }
    if (status >= 300 && status < 400) {
      const String loc = http.header("Location");
      http.end();
      if (loc.length() == 0) {
        Serial.println("[PLG] redirect without Location");
        return FetchResult::kTransportFailed;
      }
      char next[htmlform::kMaxUrl];
      if (!htmlform::resolveUrl(loc.c_str(), sUrl, next, sizeof(next))) {
        Serial.printf("[PLG] redirect target unsupported: %s\n", loc.c_str());
        return FetchResult::kTransportFailed;
      }
      std::memcpy(sUrl, next, std::strlen(next) + 1);
      continue;
    }
    BodySink sink(sPage, kPageCapacity);
    http.writeToStream(&sink);
    http.end();
    if (sink.overflow()) {
      Serial.println("[PLG] page over cap, rejected whole");
      return FetchResult::kTransportFailed;
    }
    sPageLen = sink.length();
    if (captive::classify(status, sPageLen) == captive::kOpen) return FetchResult::kAlreadyOpen;
    return FetchResult::kHavePage;
  }
  Serial.println("[PLG] redirect budget exhausted");
  return FetchResult::kTransportFailed;
}

// Re-probe after the submit: the plan's arbiter (exact empty 204 = joined).
bool reprobeOpen() {
  HTTPClient http;
  http.begin(captive::kProbeUrl);
  http.setConnectTimeout(kTimeoutMs);
  http.setTimeout(kTimeoutMs);
  const int status = http.GET();
  size_t bodyBytes = 0;
  if (status > 0) {
    BodySink sink(sPage, kReprobeBodyCap); // page buffer is spent by now
    http.writeToStream(&sink);
    bodyBytes = sink.length();
  }
  http.end();
  return status > 0 && captive::classify(status, bodyBytes) == captive::kOpen;
}

} // namespace

void PortalLoginService::init(const settings::Settings& settings, const WifiService& wifi) {
  settings_ = &settings;
  wifi_ = &wifi;
}

bool PortalLoginService::consumePromptEdge() {
  if (settings_ == nullptr || wifi_ == nullptr) {
    return false; // init never ran (provisioning boot): stay inert
  }
  const bool up = wifi_->connected();
  if (!up) {
    linkWasUp_ = false;
    sessionPrompted_ = false;
    return false;
  }
  if (!linkWasUp_) {
    linkWasUp_ = true;
    sessionPrompted_ = false; // fresh link session: prompt may fire again
  }
  if (sessionPrompted_ || running_) {
    return false;
  }
  const settings::Network* active = settings::activeNetwork(*settings_);
  if (active == nullptr || active->portalState != settings::kPortalFound) {
    return false;
  }
  sessionPrompted_ = true;
  return true;
}

bool PortalLoginService::start() {
  if (running_) return false;
  running_ = true;
  if (xTaskCreate(&PortalLoginService::taskTrampoline, "portlogin", kTaskStackSize, this, 1, nullptr) != pdPASS) {
    running_ = false;
    Serial.println("[PLG] login task not created (out of memory)");
    return false;
  }
  Serial.printf("[PLG] login attempt started, heap %lu\n", static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

bool PortalLoginService::consumeFinished(bool& joined) {
  if (!finishedEdge_) return false;
  finishedEdge_ = false;
  joined = joined_;
  return true;
}

void PortalLoginService::finish(bool joined) {
  joined_ = joined;
  finishedEdge_ = true;
  running_ = false;
  vTaskDelete(nullptr);
}

void PortalLoginService::taskTrampoline(void* arg) {
  static_cast<PortalLoginService*>(arg)->run();
}

void PortalLoginService::run() {
  // 1. Land on the interception page (redirects walked manually, capped).
  std::snprintf(sUrl, sizeof(sUrl), "%s", captive::kProbeUrl);
  const FetchResult fetched = fetchInterceptionPage();
  if (fetched == FetchResult::kAlreadyOpen) {
    Serial.println("[PLG] network is open after all; no form needed");
    finish(true);
    return;
  }
  if (fetched != FetchResult::kHavePage) {
    finish(false);
    return;
  }

  // 2. First form of the page.
  if (!htmlform::parseFirstForm(sPage, sPageLen, sForm)) {
    Serial.println("[PLG] no submittable form on the interception page");
    finish(false);
    return;
  }

  // 3. Fill defaults and submit (https actions fail in buildRequest without
  //    an attempt). The submit response itself is not interpreted.
  if (!htmlform::buildRequest(sForm, sUrl, sSubmitUrl, sizeof(sSubmitUrl), sBody, sizeof(sBody))) {
    Serial.println("[PLG] request unsupported (https action) or too large");
    finish(false);
    return;
  }
  {
    HTTPClient http;
    http.begin(sSubmitUrl);
    http.setConnectTimeout(kTimeoutMs);
    http.setTimeout(kTimeoutMs);
    const int code = sForm.post ? http.POST(reinterpret_cast<uint8_t*>(sBody), std::strlen(sBody)) : http.GET();
    http.end();
    if (code <= 0) {
      Serial.printf("[PLG] submit failed (status=%d)\n", code);
      finish(false);
      return;
    }
    Serial.printf("[PLG] submitted %u field(s), status=%d\n", static_cast<unsigned>(sForm.fieldCount), code);
  }

  // 4. The re-probe decides: 204 = online.
  const bool open = reprobeOpen();
  Serial.printf("[PLG] re-probe %s, heap %lu\n", open ? "open: joined" : "still captive",
                static_cast<unsigned long>(ESP.getFreeHeap()));
  finish(open);
}
