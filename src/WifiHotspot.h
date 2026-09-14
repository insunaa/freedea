#pragma once

#include <cstddef>
#include <cstdint>

// Provisioning-only SoftAP (step 3.5): started once on a dedicated boot with
// a fresh heap, never alongside the STA stack — toggling AP mode inside a
// live session fragments C3 DRAM, and the provisioning boot is the recovery
// path for broken STA credentials. SSID is "Freedea-XXXX" (low bytes of the
// STA MAC); the password is 12 chars from a confusable-free alphabet drawn
// from the hardware RNG. The password is never logged: the screen and QR
// carry it.

class WifiHotspot {
public:
  // Starts the WPA2 AP (core defaults: channel 1, visible, 4 stations).
  // ssid()/password()/ipString() are only meaningful after success; on
  // failure no portal is served and the screen shows the inactive state.
  bool begin();

  bool active() const { return active_; }
  const char* ssid() const { return ssid_; }
  const char* password() const { return password_; }
  // IPv4 of the AP interface as a dotted quad ("192.168.4.1" default).
  const char* ipString() const { return ipString_; }

private:
  static constexpr size_t kPasswordLen = 12;
  // Excludes confusables (I, L, O, 0, 1).
  static constexpr const char* kPasswordAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

  char ssid_[16] = {};
  char password_[kPasswordLen + 1] = {};
  char ipString_[16] = {};
  bool active_ = false;
};
