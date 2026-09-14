// Freedea — WiFi Midea AC remote for the Xteink X4 (ESP32-C3, FreeInk SDK).
//
// Boot: splash frame (name, version, battery %, fill/stroke rect pair) with
// heap probes evidencing the single 48 KB framebuffer, then the Navigation
// screen framework (ui/) takes over the loop.

#include <Arduino.h>

#include <BatteryMonitor.h>
#include <BoardConfig.h>
#include <EInkDisplay.h>
#include <FreeInkUIDisplayTarget.h>
#include <InputManager.h>
#include <PowerManager.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

#ifdef FREEDDEA_DEBUG_FS
#include <SDCardManager.h>
#endif

#include "AppVersion.h"
#include "CapabilitiesStore.h"
#include "DeviceStore.h"
#include "SettingsStore.h"
#include "WebPortal.h"
#include "WifiHotspot.h"
#include "WifiService.h"
#include "service/AcService.h"
#include "service/CaptiveProbeService.h"
#include "service/DeviceDiscovery.h"
#include "service/PortalLoginService.h"
#include "service/WeatherService.h"
#include "service/WireGuardService.h"
#include "ui/Navigation.h"

namespace ui = freeink::ui;

static constexpr int16_t kCardWidth = 240;
static constexpr int16_t kCardHeight = 80;
static constexpr int16_t kCardInset = 24;

EInkDisplay display(BoardConfig::ACTIVE.display.sclk, BoardConfig::ACTIVE.display.mosi, BoardConfig::ACTIVE.display.cs,
                    BoardConfig::ACTIVE.display.dc, BoardConfig::ACTIVE.display.rst, BoardConfig::ACTIVE.display.busy);
InputManager input;
BatteryMonitor battery;
Navigation nav(display, input);
SettingsStore settingsStore;
DeviceStore deviceStore;
CapabilitiesStore capStore;
WifiService wifi;
WifiHotspot hotspot;
WebPortal portal;
AcService acService;
DeviceDiscovery deviceDiscovery;
WeatherService weatherService;
CaptiveProbeService captiveProbe;
PortalLoginService portalLogin;
WireGuardService wgService;

// Repaint when the link state changes: the connectivity banner (5.1) rides
// every screen, as does the Settings status row. The provisioning Hotspot
// screen is excluded — the radio is off by design there (link stays
// kDisabled, no banner), and it must not be disturbed mid-provisioning. Runs
// from wifi.tick(), before nav.tick(), so the repaint lands in the same loop
// iteration.
static void wifiLinkChanged(void* ctx, WifiService::Link) {
  Navigation* navigation = static_cast<Navigation*>(ctx);
  if (navigation->current() != ScreenId::Hotspot) navigation->requestRender();
}

// Captive probe (7.2c) persist hook: the verdict is applied to the settings
// on the main loop by consumeResult(); this only schedules the debounced
// save, so the SD write stays single-task.
static void captiveProbePersist(void* ctx) {
  static_cast<SettingsStore*>(ctx)->markDirty();
}

// 5.2 timing guard: the UI path must never block on I/O — socket waits live
// on the acsvc task, and any SD write here is a debounced one-off. A pass
// where one call exceeds the limit prints, exposing stalls without flooding.
// nav.tick()'s e-ink draw is legitimately ~0.5-1.7 s (display only, buttons
// keep queueing through it via the async input task), so it gets its own,
// high limit; anything past that is a wedge.
static void noteSlow(const char* what, uint32_t startUs, uint32_t limitMs) {
  const uint32_t us = micros() - startUs;
  if (us >= limitMs * 1000UL) {
    Serial.printf("[UI ] slow %s: %lu ms\n", what, static_cast<unsigned long>(us / 1000UL));
  }
}

// R.4.2 boot profile: free-heap trace per init stage (debug builds only;
// FREEDDEA_DEBUG_MEM). The first call establishes the baseline (delta 0).
#ifdef FREEDDEA_DEBUG_MEM
static void memStage(const char* stage) {
  const uint32_t freeHeap = ESP.getFreeHeap();
  static uint32_t prevFreeHeap = freeHeap;
  Serial.printf("[MEM] %-14s free %6lu (delta %+ld)\n", stage, static_cast<unsigned long>(freeHeap),
                static_cast<long>(freeHeap) - static_cast<long>(prevFreeHeap));
  prevFreeHeap = freeHeap;
}
#define MEM_STAGE(stage) memStage(stage)
#else
#define MEM_STAGE(stage)
#endif

// Compact boot summary, kept in every build: the free-heap and largest-block
// figures once setup() completes. Field-diagnostic one-liner; the detailed
// stage trace is debug-only.
static void logBootSummary() {
  Serial.printf("[freedea] boot done: heap free %lu, largest block %lu\n",
                static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
}

// 5.4 idle CPU policy: drop 160 MHz -> 80 MHz after kIdleCpuMs of no button
// activity; restore on the next press. 80 MHz is the lowest clock the C3
// offers and the documented floor while Wi-Fi is enabled. The C3's APB stays
// at 80 MHz at either CPU clock, so UART/display-SPI/ADC timing never changes.
// The hook into Navigation::tick (setActivityHook) restores 160 MHz when the
// press is drained, before the frame it triggers rasterizes: measured at 80
// MHz the Control-screen raster costs ~490 ms vs ~200 ms at 160 MHz.
static constexpr uint32_t kIdleCpuMs = 10000;
static void applyCpuPolicy(bool activity) {
  static uint32_t lastActivityMs = 0;
  static bool idleClock = false;
  const uint32_t now = millis();
  if (activity) {
    lastActivityMs = now;
    if (idleClock) {
      setCpuFrequencyMhz(160);
      idleClock = false;
      Serial.println("[PWR ] cpu 80 -> 160 MHz");
    }
    return;
  }
  if (!idleClock && now - lastActivityMs >= kIdleCpuMs) {
    setCpuFrequencyMhz(80);
    idleClock = true;
    Serial.println("[PWR ] cpu 160 -> 80 MHz (idle)");
  }
}

// Last frame before standby cuts power: e-ink is bistable, so this survives
// with the battery disconnected and shows the charge at shutdown.
static void drawPowerOffScreen() {
  display.clearScreen();
  ui::DisplayTarget target(display.getFrameBuffer(), display.getDisplayWidth(), display.getDisplayHeight(),
                           display.getDisplayWidthBytes());
  const ui::DeviceContext device = target.deviceContext();
  ui::TextStyle style{};
  style.align = ui::TextAlign::Center;
  const int16_t lh = target.lineHeight(0);
  const int16_t top = (device.height - 3 * lh) / 2;
  target.text(ui::Rect{0, top, device.width, lh}, "Powering off", style);
  char batteryLine[32];
  snprintf(batteryLine, sizeof(batteryLine), "Battery: %u%%", battery.readPercentage());
  target.text(ui::Rect{0, top + lh, device.width, lh}, batteryLine, style);
  target.text(ui::Rect{0, top + 2 * lh, device.width, lh}, "Press power to turn on", style);
  display.displayBuffer(EInkDisplay::FULL_REFRESH);
}

// 5.4 standby: the X4 holds its battery on through power.latch0 (GPIO13, also
// asserted at boot below). Drive it LOW and pad-hold it: the hold survives
// until power-on reset, and only the power button re-bridging the latch rail
// (or USB) brings the device back — a cold boot, exactly like the hardware
// switch the stock firmware presents. On battery the chip dies milliseconds
// after the latch drops; while USB keeps it alive, wait for the held button
// and deep-sleep with wake armed on the power pin instead.
static void powerOffNow() {
  Serial.println("[PWR ] power-button hold: cutting battery latch");
  drawPowerOffScreen();
  const int8_t latch = BoardConfig::ACTIVE.power.latch0;
  if (latch >= 0) {
    const gpio_num_t pin = static_cast<gpio_num_t>(latch);
    gpio_hold_dis(pin);
    pinMode(latch, OUTPUT);
    digitalWrite(latch, LOW);
    gpio_hold_en(pin);
  } else {
    Serial.println("[PWR ] no latch pin - falling back to deep sleep");
  }
  freeink::PowerManager::deepSleepUntilPowerButton();
}

#ifdef FREEDDEA_DEBUG_FS
// Boot-time dump of the SD config directory so store persistence can be
// verified from the serial log alone. Boot-only; the transient vector of
// names is freed immediately after printing.
static void dumpFreedeaDir() {
  const std::vector<String> entries = SdMan.listFiles("/.freedea");
  Serial.printf("[FS ] /.freedea: %u entries\n", static_cast<unsigned>(entries.size()));
  for (const String& entry : entries) {
    Serial.printf("[FS ]  %s\n", entry.c_str());
  }
}
#endif

static void drawSplash() {
  display.clearScreen();
  // Landscape-native X4 panel defaults to a portrait logical frame.
  ui::DisplayTarget target(display.getFrameBuffer(), display.getDisplayWidth(), display.getDisplayHeight(),
                           display.getDisplayWidthBytes());
  const ui::DeviceContext device = target.deviceContext();

  ui::TextStyle style{};
  style.align = ui::TextAlign::Center;
  const int16_t lh = target.lineHeight(0);
  const int16_t top = (device.height - 3 * lh) / 2;

  target.text(ui::Rect{0, top, device.width, lh}, "Freedea", style);

  char versionLine[32];
  snprintf(versionLine, sizeof(versionLine), "v%s", kAppVersion);
  target.text(ui::Rect{0, top + lh, device.width, lh}, versionLine, style);

  char batteryLine[32];
  snprintf(batteryLine, sizeof(batteryLine), "Battery: %u%%", battery.readPercentage());
  target.text(ui::Rect{0, top + 2 * lh, device.width, lh}, batteryLine, style);

  // Rect bring-up: stroked outline with a solid block centered inside.
  const int16_t cardX = (device.width - kCardWidth) / 2;
  const int16_t cardY = top + 3 * lh + 24;
  target.stroke(ui::Rect{cardX, cardY, kCardWidth, kCardHeight}, ui::Paint::solid(ui::Color::Black), 2);
  target.fill(
      ui::Rect{cardX + kCardInset, cardY + kCardInset, kCardWidth - 2 * kCardInset, kCardHeight - 2 * kCardInset},
      ui::Paint::solid(ui::Color::Black));

  display.displayBuffer(EInkDisplay::FULL_REFRESH);
}

void setup() {
  // Own the battery MOSFET latch before anything else (SDK contract): on the
  // self-latching X4 revisions driving it HIGH is a no-op, on the revisions
  // that only stay powered while the button is held it is what keeps the
  // board on, and it puts the pin under firmware drive so standby
  // (powerOffNow) can drive it LOW.
  BoardConfig::holdPowerRails();
  Serial.begin(115200);
  MEM_STAGE("entry");
  // SD mounts before the display: the display is write-only, so EpdBus begins
  // the shared SPI bus with MISO unattached, and arduino-esp32's SPIClass::begin()
  // ignores the pins of every call after the first. Mounting SD first wins
  // MISO (GPIO7) into the bus pinmap; the display's later begin() is a no-op.
  nav.setActivityHook(applyCpuPolicy);
  settingsStore.init();
  deviceStore.init(settingsStore.sdReady());
  capStore.init(settingsStore.sdReady());
  MEM_STAGE("storage");
#ifdef FREEDDEA_DEBUG_FS
  if (settingsStore.sdReady()) {
    dumpFreedeaDir();
  }
#endif
  display.begin();
  MEM_STAGE("display");
  input.begin();
  MEM_STAGE("input");
  // Async input (responsiveness): a priority-2 task samples the buttons every
  // 15 ms and latches press edges into a queue Navigation::tick() drains with
  // popPress(). Without it, a press landing inside the blocking e-ink refresh
  // (~0.5-1.7 s) is sampled away. One-time cost: 4 KB task stack + five
  // depth-8 queues (~0.5 KB); heap delta logged by the stage trace.
  input.beginAsync(2, 15, 8);
  MEM_STAGE("async input");
  drawSplash();
  MEM_STAGE("splash");
  // After the display stages so their deltas stay display-only.
  // Subscribe before init() so the first kConnecting transition is reported;
  // a requestRender() before nav.start() is harmless (start() renders anyway).
  wifi.setLinkCallback(wifiLinkChanged, &nav);
  // Provisioning boot (step 3.5): the AP runs instead of the STA stack —
  // the two never coexist on the C3. Entry paths: no SSID configured (fresh
  // device) or the Settings -> Hotspot row, which wrote the once-consumed
  // flag before rebooting.
  if (settingsStore.consumeProvisionFlag() || !settings::anyNetwork(settingsStore.settings())) {
    Serial.println("[freedea] provisioning boot (AP only, STA stays off)");
    if (hotspot.begin()) {
      // The provisioning listener never idles out (6.3): it is the
      // lockout-recovery path.
      portal.begin(settingsStore, deviceStore, false);
    }
    // Root screen of a provisioning boot; Back reboots into a normal boot
    // (flag already consumed, Settings -> Hotspot re-enters if needed).
    MEM_STAGE("hotspot");
    nav.start(ScreenId::Hotspot);
    logBootSummary();
    return;
  }
  // Connect is async: the radio associates while the UI comes up.
  wifi.init(settingsStore.settings());
  MEM_STAGE("wifi");
  // The STA-mode web portal stays closed at boot (6.3): Settings -> Web
  // portal opens it on demand and it closes itself after 15 idle minutes.
  // The always-listening port-80 surface is gone; with no SSID the SD
  // settings file is the entry path (and the radio stays off).
  // AC service: starts only on a normal boot; a provisioning boot never talks
  // to the AC. Inert on failure; the UI shows "no data". The capabilities
  // cache for the target seeds the service so a reboot skips the query (4.5).
  midea::AcCapabilities cachedCaps;
  const devices::Device* target = AcService::selectTarget(deviceStore.list());
  const midea::AcCapabilities* capsSeed = nullptr;
  if (target != nullptr && capStore.load(target->id, cachedCaps)) {
    capsSeed = &cachedCaps;
  }
  acService.init(deviceStore.list(), wifi, settingsStore.settings(), capsSeed);
  MEM_STAGE("ac service");
  // Weather fetch task: inert while settings.weather is disabled or missing
  // a location (the wake costs one branch per second).
  weatherService.init(settingsStore.settings(), wifi);
  MEM_STAGE("weather");
  // Captive probe (7.2c): one connectivity check per link session while the
  // active profile has no cached portal state; the verdict lands in the
  // profile and the debounced save is scheduled from the main loop.
  captiveProbe.init(settingsStore.settings(), wifi, &captiveProbePersist, &settingsStore);
  MEM_STAGE("captive probe");
  // Captive portal auto-login (7.2d): prompts once per link session when the
  // active profile caches a portal; the attempt runs on demand.
  portalLogin.init(settingsStore.settings(), wifi);
  MEM_STAGE("portal login");
  // WireGuard client (7.3): inert until the settings block is enabled and
  // complete; the tunnel then comes up after associate.
  wgService.init(settingsStore.settings(), wifi);
  MEM_STAGE("wireguard");
  nav.start();
  MEM_STAGE("nav start");
  logBootSummary();
}

void loop() {
  // 5.4.3 battery clock: one ADC sample per minute (the GPIO0 divider is on
  // ADC1, safe beside Wi-Fi, and the ~1 ms read stays off the draw path —
  // worst case against noteSlow's 100 ms budget is everything else in this
  // loop). The deadband lookup keeps the on-screen number from oscillating
  // at notch boundaries (>100 previous = no history); the corner label
  // updates on the next natural repaint rather than forcing one.
  static uint32_t lastBatteryMs = 0;
  static uint16_t batteryPct = 0xFFFF;
  if (batteryPct == 0xFFFF || millis() - lastBatteryMs >= 60000) {
    const bool firstSample = batteryPct == 0xFFFF;
    lastBatteryMs = millis();
    batteryPct = BatteryMonitor::percentageFromMillivolts(battery.readMillivolts(), batteryPct);
    setBatteryPercent(batteryPct);
    // The boot frame rendered before this first sample; show the label now.
    if (firstSample) nav.requestRender();
  }
#ifdef FREEDDEA_DEBUG_MEM
  // R.4.2 free-heap floor: 1 s sample, running minimum tracked across the
  // session. Navigate to the worst screen (Control, 8 rows) and read the
  // floor off the serial log. Samples land between loop iterations, so a
  // transient allocation that is freed within one iteration can dip below
  // the reported floor.
  static uint32_t heapSampleMs = 0;
  static uint32_t heapFloor = UINT32_MAX;
  if (millis() - heapSampleMs >= 1000) {
    heapSampleMs = millis();
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < heapFloor) heapFloor = freeHeap;
    Serial.printf("[MEM] free %6lu floor %6lu largest %6lu\n", static_cast<unsigned long>(freeHeap),
                  static_cast<unsigned long>(heapFloor),
                  static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT)));
  }
#endif
  uint32_t t0 = micros();
  settingsStore.tick();
  noteSlow("settings", t0, 100);
  t0 = micros();
  wifi.tick();
  noteSlow("wifi", t0, 100);
  t0 = micros();
  // Captive probe (7.2c): consume first (the verdict write happens here),
  // then let the spawn logic see this iteration's link state. The repaint
  // only matters on the two screens that show the cached state.
  if (captiveProbe.consumeResult() && (nav.current() == ScreenId::Settings || nav.current() == ScreenId::Networks)) {
    nav.requestRender();
  }
  captiveProbe.tick();
  noteSlow("captive", t0, 100);
  // WireGuard (7.3): consume + state machine are main-loop only (the
  // bring-up task just reports); repaint the row when the tunnel state moves.
  t0 = micros();
  wgService.tick();
  if (wgService.consumeStatusEdge() && nav.current() == ScreenId::Settings) {
    nav.requestRender();
  }
  noteSlow("wg", t0, 100);
  // Portal auto-login (7.2d): the attempt's verdict surfaces as a toast and
  // pops the prompt screen; the prompt edge pushes it (never stacking a
  // second copy on top of itself).
  bool portalJoined = false;
  if (portalLogin.consumeFinished(portalJoined)) {
    showToast(portalJoined ? "Portal login complete" : "Could not join network");
    if (nav.current() == ScreenId::CaptiveLogin) nav.pop();
    nav.requestRender();
  }
  if (portalLogin.consumePromptEdge() && nav.current() != ScreenId::CaptiveLogin) {
    nav.push(ScreenId::CaptiveLogin);
  }
  t0 = micros();
  portal.tick();
  noteSlow("portal", t0, 100);
  // AC poll mode (6.1c/d): the screen actually showing AC data decides what
  // the service may poll — Dashboard adds the group-7 power row, Control is
  // state-only, Details runs the full six-group round every 3 s; everything
  // else (Home, Settings, About, Devices, Hotspot) stays passive: TCP
  // keepalive on the socket, no polling. Entering a polling mode refreshes
  // immediately.
  {
    const ScreenId screen = nav.current();
    // Dropdown open: pause polling entirely. A state-change edge landing
    // mid-stepping would steal a full ~1 s repaint between button presses;
    // command sends go through the task queue regardless of poll mode. An
    // armed button lock overrides kNone inside the service (it must keep
    // watching for pet presses), and its frozen snapshot raises no edges.
    AcService::PollMode mode = AcService::PollMode::kNone;
    if (screen == ScreenId::Dashboard) {
      mode = AcService::PollMode::kStatePower;
    } else if (screen == ScreenId::Control) {
      mode = controlDropdownOpen() ? AcService::PollMode::kNone : AcService::PollMode::kState;
    } else if (screen == ScreenId::Details) {
      mode = AcService::PollMode::kStateAll;
    }
    acService.setPollMode(mode);
  }
  // AC state change (4.7: includes the post-send GetState of a control
  // command): reseed the Control screen's composition base, then repaint the
  // Dashboard/Control, but only while one shows (same e-ink gate as the WiFi
  // link callback). The edge flag lets the acsvc task signal without
  // touching Navigation cross-task.
  if (acService.consumeStateChanged()) {
    onAcStateChanged();
    if (nav.current() == ScreenId::Dashboard || nav.current() == ScreenId::Control ||
        nav.current() == ScreenId::Details) {
      nav.requestRender();
    }
  }
  // A group round completed whose parsed values moved (6.1d; 9.2: the service
  // raises the edge per round and only on a value change, not per group);
  // Details repaints, while the Dashboard follows e-ink change-gating like
  // the state edge above and repaints only when its power row's value
  // actually moved. Consumed unconditionally so the edge never lingers for
  // other screens.
  if (acService.consumeExtUpdate()) {
    if (nav.current() == ScreenId::Details) {
      nav.requestRender();
    } else if (nav.current() == ScreenId::Dashboard) {
      // 0xFFFFFFFF = "no value"; above any uint16_t reading.
      static uint32_t lastDrawnPower = 0xFFFFFFFF;
      const std::optional<uint16_t> power = acService.outdoorUnitPowerW();
      const uint32_t value = power ? static_cast<uint32_t>(*power) : 0xFFFFFFFF;
      if (value != lastDrawnPower) {
        lastDrawnPower = value;
        nav.requestRender();
      }
    }
  }
  // AC session up/down transition (5.1): repaint the screens that show the
  // stale indicator (same e-ink gate as the state edge above).
  if (acService.consumeReachabilityEdge()) {
    if (nav.current() == ScreenId::Dashboard || nav.current() == ScreenId::Control ||
        nav.current() == ScreenId::Details) {
      nav.requestRender();
    }
  }
  // Protocol/session errors (5.2): surface as the bottom-strip toast on
  // whatever screen is up. The acsvc task never touches the UI — it fills a
  // latest-wins slot (one message per occurrence, never per backoff retry)
  // that is drained here; expiry schedules the single clearing repaint.
  char acError[40];
  if (acService.consumeError(acError, sizeof(acError))) {
    showToast(acError);
    nav.requestRender();
  }
  // Weather snapshot (6.2): repaint the Dashboard only (the panel's screen)
  // when a fetch published new data; failures surface as a toast.
  if (weatherService.consumeUpdate()) {
    if (nav.current() == ScreenId::Dashboard || nav.current() == ScreenId::Settings) {
      nav.requestRender();
    }
  }
  char wxError[40];
  if (weatherService.consumeError(wxError, sizeof(wxError))) {
    showToast(wxError);
    nav.requestRender();
  }
  if (consumeToastExpiry()) {
    nav.requestRender();
  }
  // Staleness clock (5.1): the Dashboard's "last update m:ss ago" counter
  // ages without events, so while it is displayed over a dead session,
  // repaint every ~10 s to keep it truthful. Nothing else repaints during an
  // outage; the refresh ladder absorbs the extra fast draws.
  if ((nav.current() == ScreenId::Dashboard || nav.current() == ScreenId::Details) && acService.hasState() &&
      !acService.sessionUp()) {
    static uint32_t lastStaleTickMs = 0;
    if (millis() - lastStaleTickMs >= 10000) {
      lastStaleTickMs = millis();
      nav.requestRender();
    }
  }
  // Capabilities arrived from the device (4.5): persist the SD cache here in
  // the main loop — SdFat is not task-safe, so the acsvc task never touches
  // it — and repaint the Control screen, whose options gate on them.
  if (acService.consumeCapabilitiesEdge()) {
    t0 = micros();
    capStore.save(acService.targetId(), acService.capabilities());
    noteSlow("capsave", t0, 100);
    if (nav.current() == ScreenId::Control) {
      nav.requestRender();
    }
  }
  // Property snapshot changed (8.3): the Control rows for iECO/out-silent
  // show acknowledged property values (post-set re-query, or the bring-up
  // GET that first makes a row visible). Consumed unconditionally so the
  // edge never lingers for other screens.
  if (acService.consumePropsEdge()) {
    if (nav.current() == ScreenId::Control) {
      nav.requestRender();
    }
  }
  // Discovery scan driver: non-blocking unless a scan is active (4.4). The
  // done edge repaints once per scan, not per datagram (e-ink discipline).
  t0 = micros();
  deviceDiscovery.poll();
  noteSlow("discovery", t0, 100);
  if (deviceDiscovery.consumeScanDoneEdge() && nav.current() == ScreenId::Devices) {
    nav.requestRender();
  }
  t0 = micros();
  nav.tick();
  noteSlow("draw", t0, 2000);
  // Standby gesture (5.4): a >= 3 s power-button hold. Never returns on
  // battery power.
  if (nav.consumePowerOff()) {
    powerOffNow();
  }
  applyCpuPolicy(nav.consumeActivity());
  delay(30);
}
