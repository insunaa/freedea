#pragma once

// LAN portal (steps 3.3 + 3.4d + 3.5d + 6.3 + 7.2b): a single page on port
// 80 with a WiFi network list (per-profile edit/connect/remove forms plus an
// add form) and a Midea device add/update form, built on the stock core
// WebServer. In arduino-esp32 3.x that class is a plain
// NetworkServer socket implementation — no extra task; handleClient() serves
// requests from the main loop, so the portal's steady cost is one listening
// socket plus transient per-request Strings. No authentication: this is a
// trusted-LAN onboarding tool (the provisioning AP is WPA2-protected).
// Normal boots start closed: Settings -> Web portal opens the listener on
// demand and it closes itself after 15 idle minutes. The provisioning AP
// boot always listens — it is the lockout-recovery path. Radio-affecting
// saves (device saves; network saves that change the active profile's
// credentials, the active pointer, or remove the active profile) are
// persist-to-SD followed by ESP.restart() — the reboot is the atomic
// apply/commit boundary, keeps AP and STA from ever coexisting, and returns
// the STA portal to closed. Network edits to a non-active profile apply
// without a reboot (the radio is untouched, like the weather form).

#include <WebServer.h>

#include <memory>

class SettingsStore;
class DeviceStore;

class WebPortal {
public:
  // All references must outlive the portal (main.cpp globals). Idempotent:
  // an already-listening portal keeps its settings. Returns false only if the
  // server object could not be allocated; the device still boots. With
  // closeOnIdle, tick() closes the portal after kIdleTimeoutMs without a
  // request; the provisioning boot passes false so the recovery listener
  // never lapses.
  bool begin(SettingsStore& settingsStore, DeviceStore& deviceStore, bool closeOnIdle);

  // Close the listener and free the server; safe when already closed.
  void end();

  bool enabled() const { return server_ != nullptr; }

  // Call from the main loop; services at most one pending request per call
  // and enforces the idle timeout when begin() asked for it.
  void tick();

private:
  static constexpr uint16_t kPort = 80;
  static constexpr uint32_t kIdleTimeoutMs = 15UL * 60UL * 1000UL;

  // Stamp the last-request clock (called from every route handler).
  void noteRequest() { lastActivityMs_ = millis(); }

  void handleRoot();         // GET /: network list + device forms and stored device list
  void handleNetwork();      // POST /net: save/add/connect/remove one network profile
  void handleDeviceSave();   // POST /device: add or update one Midea device, reboot
  void handleDeviceRemove(); // POST /remove: drop one device by id, reboot
  void handleWeather();      // POST /weather: validate + persist weather config (no reboot)
  void handleWireGuard();    // POST /wireguard: validate + persist WireGuard config (no reboot, 7.3)

  std::unique_ptr<WebServer> server_;
  uint32_t lastActivityMs_ = 0;
  bool closeOnIdle_ = false;
  SettingsStore* store_ = nullptr;
  DeviceStore* deviceStore_ = nullptr;
};
