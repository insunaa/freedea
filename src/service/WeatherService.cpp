// Freedea — WeatherService task (Phase 6.2). See WeatherService.h.

#include "WeatherService.h"

#include <Arduino.h>
#include <HTTPClient.h>

#include <cstring>
#include <type_traits>

#include "../WifiService.h"

static_assert(std::is_trivially_copyable<WeatherService::Snapshot>::value,
              "weather snapshot crosses tasks by value copy");

namespace {

// Write-only Stream sink for HTTPClient::writeToStream (which de-chunks the
// transfer, unlike raw stream reads). Everything beyond `cap` is counted but
// dropped; overflow marks the whole fetch as failed so a truncated body is
// never parsed.
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

// E4 coordinate to decimal for the request URL (always called with a
// configured, non-zero coordinate).
void appendCoord(char* buf, size_t cap, int32_t v) {
  const bool neg = v < 0;
  const uint32_t a = neg ? -static_cast<uint32_t>(v) : static_cast<uint32_t>(v);
  snprintf(buf, cap, "%s%u.%04u", neg ? "-" : "", static_cast<unsigned>(a / 10000u), static_cast<unsigned>(a % 10000u));
}

} // namespace

bool WeatherService::init(const settings::Settings& settings, const WifiService& wifi) {
  const uint32_t heapBefore = ESP.getFreeHeap();
  settings_ = &settings;
  wifi_ = &wifi;
  mutex_ = xSemaphoreCreateMutex();
  const BaseType_t created = mutex_ != nullptr && xTaskCreate(&WeatherService::taskTrampoline, "weather",
                                                              kTaskStackSize, this, 1, nullptr) == pdPASS;
  Serial.printf("[WX ] init: heap %lu -> %lu\n", static_cast<unsigned long>(heapBefore),
                static_cast<unsigned long>(ESP.getFreeHeap()));
  if (created == pdFALSE) {
    Serial.println("[WX ] init FAILED (mutex/task); weather stays inert");
  }
  return created != pdFALSE;
}

void WeatherService::taskTrampoline(void* arg) {
  static_cast<WeatherService*>(arg)->run();
}

void WeatherService::run() {
  Serial.println("[WX ] task started");
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kWakeMs));
    // Live config read (benign tearing, same convention as WifiService):
    // disabled or unconfigured costs one branch per second.
    if (settings_ == nullptr || !settings_->weather.enabled || !settings::weatherConfigured(settings_->weather)) {
      continue;
    }
    if (wifi_ == nullptr || !wifi_->connected()) {
      continue; // no link: no attempt, so WiFi recovery triggers the boot-style fetch
    }
    const uint32_t now = millis();
    const bool due = !everAttempted_ || now - lastAttemptMs_ >= kFetchIntervalMs;
    if (!due && !refreshRequested_) {
      continue;
    }
    refreshRequested_ = false;
    everAttempted_ = true;
    lastAttemptMs_ = now; // failures also wait out the interval: no retry storm
    fetchOnce();
  }
}

void WeatherService::fetchOnce() {
  // Static like the other task scratch: 2 KB would blow the stack budget.
  static char sBody[kBodyCapacity + 1];

  const settings::Weather& w = settings_->weather;
  char latStr[16];
  char lonStr[16];
  appendCoord(latStr, sizeof(latStr), w.latE4);
  appendCoord(lonStr, sizeof(lonStr), w.lonE4);
  char url[256];
  snprintf(url, sizeof(url),
           "http://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
           "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,wind_speed_10m"
           "&hourly=temperature_2m&forecast_hours=12&timezone=auto",
           latStr, lonStr);

  const uint32_t heapBefore = ESP.getFreeHeap();
  HTTPClient http;
  http.begin(url);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  // Follow-redirects stays disabled: any 3xx (including a captive-portal
  // detour) is a failure for us, never a page to parse.
  const int status = http.GET();
  if (status != 200) {
    Serial.printf("[WX ] GET failed status=%d, keeping last snapshot\n", status);
    reportError(status < 0 ? "weather: connection failed" : "weather: bad status");
    http.end();
    return;
  }

  BodySink sink(sBody, kBodyCapacity);
  http.writeToStream(&sink);
  http.end();

  if (sink.overflow()) {
    Serial.printf("[WX ] body exceeded %u B, discarded\n", static_cast<unsigned>(kBodyCapacity));
    reportError("weather: response too large");
    return;
  }
  if (sink.length() == 0) {
    Serial.println("[WX ] empty body");
    reportError("weather: empty response");
    return;
  }
  sBody[sink.length()] = '\0';

  weather::Snapshot snap;
  if (!weather::parseResponse(sBody, sink.length(), snap)) {
    // Dump a sanitized slice of the head: the usual culprits are redirect
    // pages or captive-portal HTML, both identifiable from the first bytes.
    char head[73];
    size_t n = sink.length() < sizeof(head) - 1 ? sink.length() : sizeof(head) - 1;
    for (size_t i = 0; i < n; ++i) {
      const char c = sBody[i];
      head[i] = (c >= ' ' && c < 127) ? c : '.';
    }
    head[n] = '\0';
    Serial.printf("[WX ] parse failed (%u B): %s\n", static_cast<unsigned>(sink.length()), head);
    reportError("weather: bad response");
    return;
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  snapshot_.data = snap;
  snapshot_.atMs = millis();
  hasSnapshot_ = true;
  updateEdge_ = true;
  xSemaphoreGive(mutex_);
  Serial.printf("[WX ] ok temp=%d code=%u hum=%u wind=%u hourly=%u heap %lu->%lu\n", static_cast<int>(snap.tempC),
                static_cast<unsigned>(snap.code), static_cast<unsigned>(snap.humidity),
                static_cast<unsigned>(snap.windKmh), static_cast<unsigned>(snap.hourlyCount),
                static_cast<unsigned long>(heapBefore), static_cast<unsigned long>(ESP.getFreeHeap()));
}

bool WeatherService::snapshot(Snapshot& out) const {
  if (mutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool have = hasSnapshot_;
  if (have) {
    out = snapshot_;
  }
  xSemaphoreGive(mutex_);
  return have;
}

uint32_t WeatherService::lastFetchAgeMs() const {
  if (mutex_ == nullptr) {
    return 0xFFFFFFFFu;
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const uint32_t age = hasSnapshot_ ? millis() - snapshot_.atMs : 0xFFFFFFFFu;
  xSemaphoreGive(mutex_);
  return age;
}

bool WeatherService::consumeUpdate() {
  if (mutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool update = updateEdge_;
  updateEdge_ = false;
  xSemaphoreGive(mutex_);
  return update;
}

bool WeatherService::consumeError(char* buf, size_t cap) {
  if (mutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool has = errorEdge_;
  if (has) {
    snprintf(buf, cap, "%s", errorMsg_);
    errorEdge_ = false;
  }
  xSemaphoreGive(mutex_);
  return has;
}

void WeatherService::reportError(const char* msg) {
  if (mutex_ == nullptr) {
    return;
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  snprintf(errorMsg_, sizeof(errorMsg_), "%s", msg);
  errorEdge_ = true;
  xSemaphoreGive(mutex_);
}
