#include "WifiHotspot.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_system.h>

#include <cstring>

bool WifiHotspot::begin() {
  const uint32_t heapBefore = ESP.getFreeHeap();

  // AP-only boot: the STA side never comes up. persistent(false) keeps the
  // core from mirroring config into NVS (duplicated secrets, flash wear).
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);

  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(ssid_, sizeof(ssid_), "Freedea-%02X%02X", mac[4], mac[5]);

  const size_t alphabetLen = strlen(kPasswordAlphabet);
  for (size_t i = 0; i < kPasswordLen; i++) {
    // esp_random() % 31 has a sliver of modulo bias, immaterial for a
    // single-use password on a short-lived hotspot.
    password_[i] = kPasswordAlphabet[esp_random() % alphabetLen];
  }
  password_[kPasswordLen] = '\0';

  // Passphrase >= 8 chars selects WPA2 in the core.
  if (!WiFi.softAP(ssid_, password_)) {
    Serial.println("[AP] softAP failed, hotspot unavailable");
    return false;
  }

  const IPAddress ip = WiFi.softAPIP();
  snprintf(ipString_, sizeof(ipString_), "%u.%u.%u.%u", static_cast<uint8_t>(ip[0]), static_cast<uint8_t>(ip[1]),
           static_cast<uint8_t>(ip[2]), static_cast<uint8_t>(ip[3]));
  active_ = true;
  // Password deliberately absent from the log.
  Serial.printf("[AP] ssid=%s ip=%s heap %u -> %u\n", ssid_, ipString_, heapBefore, ESP.getFreeHeap());
  return true;
}
