#pragma once

#include <stdint.h>

namespace freeink::ui {
class DisplayTarget;
struct DeviceContext;
} // namespace freeink::ui

class Navigation;

// Screens as of step 6.1d: Dashboard and Control render live state from the
// AcService snapshot (placeholder before the first state, stale indicator
// with the age of the last update while the AC session is down); Control
// edits compose SetState commands for the service. Devices lists saved
// devices and runs the on-demand LAN discovery scan; the Settings screen
// shows the live WiFi link status; Hotspot is the root screen of a
// provisioning boot (QR + AP credentials); Details is the scrolling
// group-data readout reached from the Dashboard; Licenses lists the bundled
// open-source components and LicenseText shows the selected full license
// text (6.4). Networks lists the saved WiFi profiles and switches the active
// one (7.2b); CaptiveLogin prompts the captive-portal auto-login (7.2d).
// drawStatusBanner() overlays the WiFi-down banner on every screen.
enum class ScreenId : uint8_t {
  Home,
  Dashboard,
  Control,
  Settings,
  About,
  Hotspot,
  Devices,
  Details,
  Licenses,
  LicenseText,
  Networks,
  CaptiveLogin
};

// Registry entry: a screen draws itself into the display target and consumes
// logical InputManager button events. render() receives the freshly built
// target for the current frame; onButton() may mutate navigation state
// (push/pop) or the screen's own state plus Navigation::requestRender().
struct ScreenHandlers {
  const char* title;
  void (*render)(freeink::ui::DisplayTarget& target, const freeink::ui::DeviceContext& device, Navigation& nav);
  void (*onButton)(Navigation& nav, uint8_t button);
};

const ScreenHandlers& screenFor(ScreenId id);

// Called from the main loop when AcService reports a changed (acknowledged)
// state: reseeds the Control screen's edit-composition base. Safe before the
// first state (reseeds the default).
void onAcStateChanged();

// True while the Control screen's options dropdown is open. The main loop
// keeps the AC poll paused (kNone) for the duration: a state edge landing
// mid-stepping would steal a full ~1 s repaint between button presses.
bool controlDropdownOpen();

// Global WiFi-down banner (5.1): drawn by Navigation after each screen's
// render; a no-op while the link is connected or the radio is off. Also
// overlays the active error toast (5.2) along the bottom margin.
void drawStatusBanner(freeink::ui::DisplayTarget& target, const freeink::ui::DeviceContext& device);

// Button hint bar (7.1a): the shared, emergent footer. A screen declares the
// per-frame action of each logical button from render(); Navigation::
// renderCurrent() resets the scratch before each render and draws the bar
// after it, so no screen composes footer text. Chip slots are the physical
// seesaw bar HALVES: bottom-left bar = Back (left) | Confirm (right),
// bottom-right bar = Left (left) | Right (right) — or Up | Down on screens
// that alias side navigation onto that bar. Chips are fixed-size, anchored
// to their own half (a lone chip still sits over its switch), flush with the
// bottom screen edge with the bottom border left open: tabs rising out of
// the edge, the crosspoint reference. The side Up/Down bar itself is never
// hinted (device edge — chips there would float over the Left|Right bar;
// user 2026-09-09). A null label hides its chip; an all-null set draws no
// bar at all.
struct ButtonHints {
  const char* back = nullptr;
  const char* confirm = nullptr;
  const char* left = nullptr;
  const char* right = nullptr;
  // Right-bar labels for screens that alias Left/Right onto the side bar's
  // Up/Down behavior (crosspoint pattern); ignored when left/right are set.
  const char* up = nullptr;
  const char* down = nullptr;
};

// Called from a screen's render() (designated initializers must follow field
// declaration order, C++20).
void setButtonHints(const ButtonHints& hints);

// Draws the chip bar in the bottom band (above the toast strip); called by
// Navigation after each screen render.
void drawButtonHints(freeink::ui::DisplayTarget& target, const freeink::ui::DeviceContext& device);

// Error toast (5.2): a one-line inverted strip in the bottom margin, drawn
// by drawStatusBanner for kToastMs after showToast(). Main-loop-only state:
// the loop calls showToast() when a service reports an error, and calls
// consumeToastExpiry() every pass to trigger the single clearing repaint when
// the toast ages out. showToast() copies the message; callers keep ownership.
void showToast(const char* message);
// True once on the pass where an active toast expires (clears it).
bool consumeToastExpiry();

// Battery indicator (5.4.3): a right-aligned "N%" in the top-right corner of
// every screen, drawn by drawStatusBanner. Main-loop-only state: the loop
// samples the battery on its own cadence and calls setBatteryPercent(); the
// label rides whatever repaint comes next (e-ink discipline — no repaints
// for a 1% drift). Hidden until the first sample lands.
void setBatteryPercent(uint16_t percent);
