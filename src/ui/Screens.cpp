#include "Screens.h"

#include <Arduino.h>

#include <FreeInkUICore.h>
#include <FreeInkUIDisplayTarget.h>
#include <FreeInkUIIcon.h>
#include <InputManager.h>
#include <Settings.h>
#include <qrcodegen.h>

#include <cinttypes>
#include <cstring>

#include "../AppVersion.h"
#include "../DeviceStore.h"
#include "../SettingsStore.h"
#include "../WebPortal.h"
#include "../WifiHotspot.h"
#include "../WifiService.h"
#include "../service/AcService.h"
#include "../service/DeviceDiscovery.h"
#include "../service/PortalLoginService.h"
#include "../service/WeatherService.h"
#include "../service/WireGuardService.h"
#include "LicenseTexts.h"
#include "Navigation.h"
#include "UiFonts.h"
#include "icons_gen.h"

namespace ui = freeink::ui;

// The single WifiService instance owned by main.cpp; the settings screen
// renders its live link state.
extern WifiService wifi;

// Owned by main.cpp; the Hotspot row writes the one-shot provisioning flag
// through it before rebooting.
extern SettingsStore settingsStore;

// Owned by main.cpp; rendered by the Hotspot screen during a provisioning boot.
extern WifiHotspot hotspot;

// Owned by main.cpp; the Dashboard reads its state snapshot (4.1 read path).
extern AcService acService;

// Owned by main.cpp; the Devices screen lists its records and drives the
// on-demand discovery scan (4.4).
extern DeviceStore deviceStore;
extern DeviceDiscovery deviceDiscovery;

// Owned by main.cpp; the Settings screen's Weather row triggers a manual
// refresh (6.2), the Dashboard renders the snapshot.
extern WeatherService weatherService;

// Owned by main.cpp; the Settings screen's Web portal row opens/closes the
// on-demand LAN portal (6.3).
extern WebPortal portal;

// Owned by main.cpp; drives the captive-portal auto-login prompt (7.2d).
extern PortalLoginService portalLogin;

// Owned by main.cpp; the Settings screen's WireGuard row renders its tunnel
// status (7.3).
extern WireGuardService wgService;

namespace {

constexpr int16_t kMargin = 24;

// --- shared layout ----------------------------------------------------------

void drawHeader(ui::DisplayTarget& target, const ui::DeviceContext& device, const char* title) {
  ui::TextStyle style{};
  style.font = ui::FONT_SLOT_TITLE;
  style.align = ui::TextAlign::Center;
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
  target.text(ui::Rect{0, kMargin, device.width, lh}, title, style);
}

// Error-toast state (5.2): declared ahead of the button hint bar so the bar
// can rise above the toast strip while it is shown. Main-loop-only state —
// showToast/consumeToastExpiry and the drawStatusBanner overlay all run on
// the main loop, so no locking is needed. Millis comparisons are wrap-safe
// unsigned diffs.
static constexpr uint32_t kToastMs = 15000;
static char sToastText[40] = {0};
static uint32_t sToastUntilMs = 0;

// Button hint bar (7.1a): the shared, emergent footer. Screens declare the
// current frame's actions via setButtonHints(); Navigation::renderCurrent()
// draws the bar after the render returns. Chips cluster per physical seesaw
// bar (bottom-left = Back|Confirm, bottom-right = Left|Right), hugging the
// bottom screen edge so each pair sits over its bar; the left-to-right order
// inside a bar matches the bar's halves. The side Up/Down bar is
// deliberately never hinted — a chip floating there would read as belonging
// to the Left|Right bar (user, 2026-09-09).
ButtonHints sButtonHints{};

constexpr int16_t kHintEdge = 0; // chips flush with the screen bottom edge

int16_t buttonHintsBarHeight(ui::DisplayTarget& target) {
  return static_cast<int16_t>(target.lineHeight(freedea::kFontSlotCompact) + 8);
}

// Chip-bar top for the frame: at the screen edge, rising above the 5.2
// toast strip (bottom kMargin) while a toast is on screen.
int16_t buttonHintsTop(ui::DisplayTarget& target, const ui::DeviceContext& device) {
  const int16_t edge = sToastText[0] != '\0' ? kMargin : kHintEdge;
  return static_cast<int16_t>(device.height - edge - buttonHintsBarHeight(target));
}

// Centered small-font note above the chip bar, for text that is not a button
// action (Hotspot instructions / failure notice).
void drawPassiveNote(ui::DisplayTarget& target, const ui::DeviceContext& device, const char* text) {
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_SMALL);
  const int16_t y = static_cast<int16_t>(buttonHintsTop(target, device) - lh - 2);
  ui::TextStyle style{};
  style.font = ui::FONT_SLOT_SMALL;
  style.align = ui::TextAlign::Center;
  target.text(ui::Rect{0, y, device.width, lh}, text, style);
}

void drawBodyLine(ui::DisplayTarget& target, const ui::DeviceContext& device, int16_t y, const char* text) {
  ui::TextStyle style{};
  style.font = ui::FONT_SLOT_BODY;
  style.align = ui::TextAlign::Center;
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
  target.text(ui::Rect{0, y, device.width, lh}, text, style);
}

constexpr int16_t kRowPad = 16;

// Control-row leading icon column (9.1): Lucide glyph + gap, left of the row
// label. Orientation-agnostic width; the row layout itself stays adaptive.
constexpr int16_t kIconSize = 24;
constexpr int16_t kIconCol = kIconSize + 8;

// Selectable menu row (Home, Settings): filled when selected. Rows without a
// hint keep the centered body-font title. Rows with a hint use a left-aligned
// label and a right-aligned compact hint; the label also drops to the compact
// font only when that is needed to make the pair fit without overlap.
void drawMenuRow(ui::DisplayTarget& target, const ui::Rect& row, const char* label, const char* hint, bool selected) {
  if (selected) {
    target.fill(row, ui::Paint::solid(ui::Color::Black), 6);
  } else {
    target.stroke(row, ui::Paint::solid(ui::Color::Black), 2, 6);
  }

  const ui::FontId bodyFont = ui::FONT_SLOT_BODY;
  const ui::FontId compactFont = freedea::kFontSlotCompact;
  const int16_t bodyLh = target.lineHeight(bodyFont);
  const int16_t compactLh = target.lineHeight(compactFont);

  if (hint == nullptr || hint[0] == '\0') {
    ui::TextStyle item{};
    item.font = bodyFont;
    item.align = ui::TextAlign::Center;
    item.inverted = selected;
    target.text(ui::Rect{row.x, static_cast<int16_t>(row.y + (row.height - bodyLh) / 2), row.width, bodyLh}, label,
                item);
    return;
  }

  const int16_t textWidth = static_cast<int16_t>(row.width - 2 * kRowPad);
  if (textWidth <= 0) return;

  ui::TextStyle hintStyle{};
  hintStyle.font = compactFont;
  hintStyle.inverted = selected;

  // Keep a reasonable amount of the row reserved for the label even when the
  // hint is pathological; the status strings we ship fit well inside this.
  const int16_t measuredHintW = target.measureText(compactFont, hint, hintStyle).width;
  const int16_t maxHintW = static_cast<int16_t>(textWidth * 2 / 3);
  const int16_t hintW = measuredHintW < maxHintW ? measuredHintW : maxHintW;
  const int16_t labelMaxW = static_cast<int16_t>(textWidth - hintW - kRowPad);

  if (labelMaxW > 0) {
    ui::TextStyle labelStyle{};
    labelStyle.font = bodyFont;
    labelStyle.align = ui::TextAlign::Left;
    labelStyle.inverted = selected;
    int16_t labelLh = bodyLh;
    const int16_t labelW = target.measureText(bodyFont, label, labelStyle).width;
    if (labelW > labelMaxW) {
      labelStyle.font = compactFont;
      labelLh = compactLh;
    }
    target.text(ui::Rect{static_cast<int16_t>(row.x + kRowPad),
                         static_cast<int16_t>(row.y + (row.height - labelLh) / 2), labelMaxW, labelLh},
                label, labelStyle);
  }

  if (hintW > 0) {
    target.text(ui::Rect{static_cast<int16_t>(row.x + row.width - kRowPad - hintW),
                         static_cast<int16_t>(row.y + (row.height - compactLh) / 2), hintW, compactLh},
                hint, hintStyle);
  }
}

// --- Home -------------------------------------------------------------------

constexpr const char* kHomeItems[] = {"Dashboard", "Control", "Settings", "About"};
constexpr uint8_t kHomeItemCount = sizeof(kHomeItems) / sizeof(kHomeItems[0]);
constexpr ScreenId kHomeTargets[] = {ScreenId::Dashboard, ScreenId::Control, ScreenId::Settings, ScreenId::About};
static_assert(sizeof(kHomeTargets) / sizeof(kHomeTargets[0]) == kHomeItemCount, "kHomeItems must match kHomeTargets");

// Cursor persists across visits; a per-push reset would need an enter hook.
uint8_t sHomeSelection = 0;

void renderHome(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  drawHeader(target, device, "Freedea");

  constexpr int16_t kRowHeight = 64;
  constexpr int16_t kRowGap = 12;
  const int16_t top = static_cast<int16_t>(device.height / 4);
  for (uint8_t i = 0; i < kHomeItemCount; ++i) {
    const ui::Rect row{kMargin, static_cast<int16_t>(top + i * (kRowHeight + kRowGap)),
                       static_cast<int16_t>(device.width - 2 * kMargin), kRowHeight};
    drawMenuRow(target, row, kHomeItems[i], nullptr, i == sHomeSelection);
  }

  setButtonHints({.confirm = "Open", .up = "Up", .down = "Down"});
}

// Crosspoint pattern (user, 2026-09-09): on screens whose Up/Down navigate,
// the Left|Right bar moves the same thing; remapping keeps the "Up"/"Down"
// chips over that bar truthful.
void aliasSideToNav(uint8_t& button) {
  if (button == InputManager::BTN_LEFT) {
    button = InputManager::BTN_UP;
  } else if (button == InputManager::BTN_RIGHT) {
    button = InputManager::BTN_DOWN;
  }
}

void homeButton(Navigation& nav, uint8_t button) {
  aliasSideToNav(button);
  switch (button) {
    case InputManager::BTN_UP:
      sHomeSelection = static_cast<uint8_t>((sHomeSelection + kHomeItemCount - 1) % kHomeItemCount);
      nav.requestRender();
      break;
    case InputManager::BTN_DOWN:
      sHomeSelection = static_cast<uint8_t>((sHomeSelection + 1) % kHomeItemCount);
      nav.requestRender();
      break;
    case InputManager::BTN_CONFIRM:
      nav.push(kHomeTargets[sHomeSelection]);
      break;
    default:
      break; // BTN_BACK is a no-op on the root screen.
  }
}

// --- AC display model (4.7) -----------------------------------------------------

// Everything shown derives from the AcService snapshot using protocol-native
// values (mode 1..5, fan percent, swing nibble per msmart). Control composes
// edits into sControlDesired — a second edit before the ack lands builds on
// the first — and queues each result; the main loop reseeds it whenever the
// device publishes a change, so the screen converges on the device's
// acknowledged truth (which is also what every row displays).
midea::AcState sControlDesired;

// Operational mode numbers (msmart OperationalMode). "Off" is not a mode on
// the wire (power is a separate bit); it is the option-list sentinel that
// clears power instead of setting a mode.
constexpr uint8_t kModeAuto = 1;
constexpr uint8_t kModeCool = 2;
constexpr uint8_t kModeDry = 3;
constexpr uint8_t kModeHeat = 4;
constexpr uint8_t kModeFan = 5;
constexpr uint8_t kModeOffOption = 0xFF;

// SwingMode nibble values (msmart SwingMode).
constexpr uint8_t kSwingOff = 0x0;
constexpr uint8_t kSwingVertical = 0xC;
constexpr uint8_t kSwingHorizontal = 0x3;
constexpr uint8_t kSwingBoth = 0xF;

// Named fan speeds sent as the protocol percent (msmart FanSpeed; auto is
// midea::kFanSpeedAuto, max is msmart's MAX = 100).
constexpr uint8_t kFanSilent = 20;
constexpr uint8_t kFanLow = 40;
constexpr uint8_t kFanMedium = 60;
constexpr uint8_t kFanHigh = 80;
constexpr uint8_t kFanMax = 100;

constexpr uint8_t kTempMinC = 17;
constexpr uint8_t kTempMaxC = 30;

// targetTemperature <-> half-degree raw value: option lists and stepping use
// integers (0.5 °C resolution fits a uint8_t) and avoid float compares.
constexpr uint8_t tempHalfSteps(double celsius) {
  return static_cast<uint8_t>(celsius * 2.0 + 0.5);
}
constexpr double tempFromHalfSteps(uint8_t halfSteps) {
  return static_cast<double>(halfSteps) / 2.0;
}

const char* modeName(uint8_t mode) {
  switch (mode) {
    case kModeAuto:
      return "Auto";
    case kModeCool:
      return "Cool";
    case kModeDry:
      return "Dry";
    case kModeHeat:
      return "Heat";
    case kModeFan:
      return "Fan";
    default:
      return "-";
  }
}

// Known percents get names; anything else the device reports (e.g. a custom
// 55) shows as a plain percentage.
const char* fanName(uint8_t percent, char* buf, size_t bufSize) {
  switch (percent) {
    case midea::kFanSpeedAuto:
      return "Auto";
    case kFanSilent:
      return "Silent";
    case kFanLow:
      return "Low";
    case kFanMedium:
      return "Medium";
    case kFanHigh:
      return "High";
    case kFanMax:
      return "Max";
    default:
      snprintf(buf, bufSize, "%u%%", static_cast<unsigned>(percent));
      return buf;
  }
}

const char* swingName(uint8_t swing) {
  switch (swing) {
    case kSwingOff:
      return "Off";
    case kSwingVertical:
      return "Vertical";
    case kSwingHorizontal:
      return "Horizontal";
    case kSwingBoth:
      return "Both";
    default:
      return "-"; // angle-position values are settable only as the plain modes
  }
}

// Cycles an option index through [0, count), wrapping both directions.
constexpr uint8_t cycleOption(int32_t value, int32_t delta, uint8_t count) {
  return static_cast<uint8_t>((value + delta % count + count) % count);
}

// --- Dashboard ---------------------------------------------------------------

// "NN°C": the bundled NotoSans covers U+0020–007E only, so the degree mark
// is drawn as a small stroked circle between number and unit. On a filled
// (focused) row the circle must be white to stay visible.
void drawTemp(ui::DisplayTarget& target, const ui::Rect& box, int degrees, bool centered = true, bool inverted = false,
              bool half = false) {
  char num[8];
  snprintf(num, sizeof(num), half ? "%d.5" : "%d", degrees);

  ui::TextStyle style{};
  style.font = ui::FONT_SLOT_BODY;
  style.inverted = inverted;
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
  const int16_t numW = target.measureText(ui::FONT_SLOT_BODY, num, style).width;
  const int16_t unitW = target.measureText(ui::FONT_SLOT_BODY, "C", style).width;

  constexpr int16_t kDegree = 12;
  constexpr int16_t kGap = 3;
  const int16_t total = numW + kGap + kDegree + kGap + unitW;
  const int16_t x = centered ? static_cast<int16_t>(box.x + (box.width - total) / 2)
                             : static_cast<int16_t>(box.x + box.width - total);
  const int16_t y = static_cast<int16_t>(box.y + (box.height - lh) / 2);

  target.text(ui::Rect{x, y, numW, lh}, num, style);
  target.stroke(ui::Rect{static_cast<int16_t>(x + numW + kGap), static_cast<int16_t>(y + 2), kDegree, kDegree},
                ui::Paint::solid(inverted ? ui::Color::White : ui::Color::Black), 2, kDegree / 2);
  target.text(ui::Rect{static_cast<int16_t>(x + numW + kGap + kDegree + kGap), y, unitW, lh}, "C", style);
}

// Frame + label of a metric card; returns the remaining value area.
ui::Rect drawMetricCardFrame(ui::DisplayTarget& target, const ui::Rect& card, const char* label) {
  target.stroke(card, ui::Paint::solid(ui::Color::Black), 2, 6);

  ui::TextStyle labelStyle{};
  labelStyle.font = ui::FONT_SLOT_SMALL;
  labelStyle.align = ui::TextAlign::Center;
  const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);
  target.text(ui::Rect{card.x, static_cast<int16_t>(card.y + 10), card.width, smallLh}, label, labelStyle);

  return ui::Rect{card.x, static_cast<int16_t>(card.y + smallLh + 10), card.width,
                  static_cast<int16_t>(card.height - smallLh - 20)};
}

void drawMetricCard(ui::DisplayTarget& target, const ui::Rect& card, const char* label, int degrees,
                    bool half = false) {
  drawTemp(target, drawMetricCardFrame(target, card, label), degrees, true, false, half);
}

// Card for a value that has no numeric rendering ("no data").
void drawMetricCardText(ui::DisplayTarget& target, const ui::Rect& card, const char* label, const char* text) {
  const ui::Rect area = drawMetricCardFrame(target, card, label);
  ui::TextStyle style{};
  style.font = ui::FONT_SLOT_BODY;
  style.align = ui::TextAlign::Center;
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
  target.text(ui::Rect{area.x, static_cast<int16_t>(area.y + (area.height - lh) / 2), area.width, lh}, text, style);
}

// Three lines through the center at 0°/±60°; cos(30°)·r approximated by
// integer math (RISC-V has no FPU in this path).
void drawSnowflake(ui::DisplayTarget& target, int16_t cx, int16_t cy, int16_t r, const ui::Paint& ink) {
  const int16_t dx = static_cast<int16_t>(r * 866 / 1000);
  const int16_t dy = static_cast<int16_t>(r / 2);
  target.line(ui::Point{cx, static_cast<int16_t>(cy - r)}, ui::Point{cx, static_cast<int16_t>(cy + r)}, 2, ink);
  target.line(ui::Point{static_cast<int16_t>(cx - dx), static_cast<int16_t>(cy - dy)},
              ui::Point{static_cast<int16_t>(cx + dx), static_cast<int16_t>(cy + dy)}, 2, ink);
  target.line(ui::Point{static_cast<int16_t>(cx - dx), static_cast<int16_t>(cy + dy)},
              ui::Point{static_cast<int16_t>(cx + dx), static_cast<int16_t>(cy - dy)}, 2, ink);
}

void drawStatusRow(ui::DisplayTarget& target, const ui::Rect& row, const char* label, const char* value,
                   bool snowflake) {
  target.stroke(row, ui::Paint::solid(ui::Color::Black), 2, 6);

  ui::TextStyle labelStyle{};
  labelStyle.font = ui::FONT_SLOT_SMALL;
  const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);
  target.text(ui::Rect{static_cast<int16_t>(row.x + kRowPad), static_cast<int16_t>(row.y + (row.height - smallLh) / 2),
                       row.width, smallLh},
              label, labelStyle);

  ui::TextStyle valueStyle{};
  valueStyle.font = ui::FONT_SLOT_BODY;
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
  const int16_t valueW = target.measureText(ui::FONT_SLOT_BODY, value, valueStyle).width;
  const int16_t valueX = static_cast<int16_t>(row.x + row.width - kRowPad - valueW);
  target.text(ui::Rect{valueX, static_cast<int16_t>(row.y + (row.height - lh) / 2), valueW, lh}, value, valueStyle);

  if (snowflake) {
    constexpr int16_t kIconR = 11;
    drawSnowflake(target, static_cast<int16_t>(valueX - kIconR - 12), static_cast<int16_t>(row.y + row.height / 2),
                  kIconR, ui::Paint::solid(ui::Color::Black));
  }
}

// 5.1 stale-data label: a state exists but the service reports the AC
// unreachable; say how old the shown values are (elapsed uptime — there is
// no wall clock on this device).
void formatStaleText(char* buf, size_t cap) {
  const uint32_t ageS = acService.lastStateAgeMs() / 1000;
  if (ageS >= 3600) {
    snprintf(buf, cap, "stale - last update %u h ago", static_cast<unsigned>(ageS / 3600));
  } else {
    snprintf(buf, cap, "stale - last update %u:%02u ago", static_cast<unsigned>(ageS / 60),
             static_cast<unsigned>(ageS % 60));
  }
}

// --- Weather panel (6.2c) ----------------------------------------------------

constexpr int16_t kWxPadV = 6;
constexpr int16_t kWxSparkMin = 56;
constexpr int16_t kWxAxisLabelW = 36;
constexpr int16_t kWxAxisTickW = 4;
constexpr int16_t kWxAxisLabelGap = 4;

// "17.4" / "-3.3" from tenths — integer formatting (no %f on the draw path).
void fmtTenths(char* buf, size_t cap, int16_t tenths) {
  const uint16_t frac = static_cast<uint16_t>(tenths >= 0 ? tenths % 10 : -(tenths % 10));
  snprintf(buf, cap, "%d.%u", static_cast<int>(tenths / 10), frac);
}

// Height the panel needs at a given spark configuration. Extra height is not
// returned here: the dashboard lets the graph absorb the remaining vertical
// space once the fixed text and tick rows are accounted for.
int16_t weatherPanelHeight(int16_t lineLh, int16_t smallLh, bool spark, bool ticks) {
  int16_t h = 2 * kWxPadV + lineLh;
  if (spark) {
    h += 4 + kWxSparkMin;
    if (ticks) h += smallLh + 2;
  }
  return h;
}

// Current-conditions line plus a 12 h monochrome temperature graph: autoscaled
// to the window, labeled Y-axis on the left, a now marker at the left edge and
// hour ticks every 3 h. The header line uses the dedicated weather font and is
// width-clipped against the age stamp so long location/condition strings do not
// collide.
void drawWeatherPanel(ui::DisplayTarget& target, const ui::Rect& area, const WeatherService::Snapshot& snap, bool spark,
                      bool ticks) {
  const ui::FontId compactFont = freedea::kFontSlotCompact;
  const int16_t smallLh = target.lineHeight(compactFont);
  const int16_t padX = 12;
  target.stroke(area, ui::Paint::solid(ui::Color::Black), 2, 6);

  ui::TextStyle smallStyle{};
  smallStyle.font = compactFont;

  const settings::Weather& w = settingsStore.settings().weather;
  char tempBuf[12];
  char humBuf[8];
  const char* temp = nullptr;
  const char* hum = nullptr;
  if (snap.data.hasTemp) {
    fmtTenths(tempBuf, sizeof(tempBuf), snap.data.tempC);
    temp = tempBuf;
  }
  if (snap.data.hasHumidity) {
    snprintf(humBuf, sizeof(humBuf), "%u%%", static_cast<unsigned>(snap.data.humidity));
    hum = humBuf;
  }
  char line[64];
  line[0] = '\0';
  size_t off = 0;
  auto cat = [&](const char* s) {
    if (s == nullptr || *s == '\0' || off + 2 >= sizeof(line)) return;
    if (off != 0) line[off++] = ' ';
    while (*s != '\0' && off + 1 < sizeof(line))
      line[off++] = *s++;
    line[off] = '\0';
  };
  cat(w.name);
  cat(temp);
  cat(temp != nullptr ? "C" : nullptr);
  cat(snap.data.hasCode ? weather::codeLabel(snap.data.code) : "");
  cat(hum);

  const uint32_t ageS = (millis() - snap.atMs) / 1000u;
  char ageBuf[14];
  if (ageS < 60) {
    snprintf(ageBuf, sizeof(ageBuf), "now");
  } else if (ageS < 3600) {
    snprintf(ageBuf, sizeof(ageBuf), "%u min", static_cast<unsigned>(ageS / 60));
  } else {
    snprintf(ageBuf, sizeof(ageBuf), "%u h %02u", static_cast<unsigned>(ageS / 3600),
             static_cast<unsigned>((ageS % 3600) / 60));
  }
  const int16_t ageW = target.measureText(compactFont, ageBuf, smallStyle).width;

  ui::TextStyle lineStyle{};
  lineStyle.font = compactFont;
  const int16_t maxLineW = static_cast<int16_t>(area.width - 2 * padX - ageW - 10);
  if (line[0] != '\0' && maxLineW > 0) {
    target.text(
        ui::Rect{static_cast<int16_t>(area.x + padX), static_cast<int16_t>(area.y + kWxPadV), maxLineW, smallLh}, line,
        lineStyle);
  }
  target.text(ui::Rect{static_cast<int16_t>(area.x + area.width - padX - ageW), static_cast<int16_t>(area.y + kWxPadV),
                       ageW, smallLh},
              ageBuf, smallStyle);

  if (!spark || snap.data.hourlyCount != weather::kHourlyCount) return;

  const int16_t sparkY = static_cast<int16_t>(area.y + kWxPadV + smallLh + 4);
  const int16_t sh = static_cast<int16_t>(area.height - 2 * kWxPadV - smallLh - 4 - (ticks ? smallLh + 2 : 0));
  const int16_t gx0 = static_cast<int16_t>(area.x + padX + kWxAxisLabelW + 8);
  const int16_t gx1 = static_cast<int16_t>(area.x + area.width - padX - 8);
  if (sh < 10 || gx1 - gx0 < 60) return; // not enough room to be meaningful

  int16_t lo = snap.data.hourlyTenths[0];
  int16_t hi = lo;
  for (uint8_t i = 1; i < weather::kHourlyCount; ++i) {
    const int16_t v = snap.data.hourlyTenths[i];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  if (hi - lo < 10) { // flat window: center it, keep the scale division safe
    const int16_t mid = static_cast<int16_t>((hi + lo) / 2);
    hi = static_cast<int16_t>(mid + 5);
    lo = static_cast<int16_t>(mid - 5);
  }

  const ui::Paint ink = ui::Paint::solid(ui::Color::Black);
  target.line(ui::Point{gx0, sparkY}, ui::Point{gx0, static_cast<int16_t>(sparkY + sh)}, 1, ink);
  target.line(ui::Point{gx0, static_cast<int16_t>(sparkY + sh)}, ui::Point{gx1, static_cast<int16_t>(sparkY + sh)}, 1,
              ink);

  auto px = [&](uint8_t i) {
    return static_cast<int16_t>(gx0 + static_cast<int32_t>(gx1 - gx0) * i / (weather::kHourlyCount - 1));
  };
  auto py = [&](int16_t v) {
    return static_cast<int16_t>(sparkY + 1 + static_cast<int32_t>(hi - v) * (sh - 2) / (hi - lo));
  };

  if (sh >= static_cast<int16_t>(2 * smallLh + 8)) {
    char axisBuf[10];
    auto drawYLabel = [&](int16_t value, int16_t tickY) {
      fmtTenths(axisBuf, sizeof(axisBuf), value);
      const int16_t measuredW = target.measureText(compactFont, axisBuf, smallStyle).width;
      if (measuredW <= 0) return;
      int16_t ty = static_cast<int16_t>(tickY - smallLh / 2);
      const int16_t minY = static_cast<int16_t>(area.y + kWxPadV + smallLh + 6);
      const int16_t maxY = static_cast<int16_t>(sparkY + sh - smallLh);
      if (ty < minY) ty = minY;
      if (maxY > minY && ty > maxY) ty = maxY;
      const int16_t labelRight = static_cast<int16_t>(gx0 - kWxAxisTickW - kWxAxisLabelGap);
      int16_t lw = measuredW;
      int16_t lx = static_cast<int16_t>(labelRight - lw);
      if (lx < area.x + padX) lx = static_cast<int16_t>(area.x + padX);
      if (lx + lw > labelRight) lw = static_cast<int16_t>(labelRight - lx);
      if (lw <= 0) return;
      target.text(ui::Rect{lx, ty, lw, smallLh}, axisBuf, smallStyle);
      if (tickY >= sparkY && tickY < sparkY + sh) {
        target.line(ui::Point{static_cast<int16_t>(gx0 - kWxAxisTickW), tickY}, ui::Point{gx0, tickY}, 1, ink);
      }
    };

    drawYLabel(hi, py(hi));
    if (sh >= static_cast<int16_t>(3 * smallLh + 8)) {
      const int16_t mid = static_cast<int16_t>((static_cast<int32_t>(hi) + lo) / 2);
      drawYLabel(mid, py(mid));
    }
    drawYLabel(lo, py(lo));
  }

  for (uint8_t i = 0; i + 1 < weather::kHourlyCount; ++i) {
    target.line(ui::Point{px(i), py(snap.data.hourlyTenths[i])},
                ui::Point{px(static_cast<uint8_t>(i + 1)), py(snap.data.hourlyTenths[i + 1])}, 2, ink);
  }
  // "now" marker: a thicker tick on the first point (left edge = fetch hour).
  target.line(ui::Point{px(0), static_cast<int16_t>(py(snap.data.hourlyTenths[0]) - 3)},
              ui::Point{px(0), static_cast<int16_t>(py(snap.data.hourlyTenths[0]) + 3)}, 3, ink);

  if (ticks) {
    // Absolute local clock time when hourly.time was available; otherwise
    // fall back to the fetch-relative labels.
    static const char* kRelativeLabels[4] = {"now", "+3h", "+6h", "+9h"};
    const int16_t tickY = static_cast<int16_t>(sparkY + sh + 2);
    for (uint8_t j = 0; j < 4; ++j) {
      char hourBuf[8];
      const char* label = kRelativeLabels[j];
      if (snap.data.hasStartHour) {
        const uint8_t hour = static_cast<uint8_t>((snap.data.startHour + j * 3u) % 24u);
        snprintf(hourBuf, sizeof(hourBuf), "%02u:00", static_cast<unsigned>(hour));
        label = hourBuf;
      }
      const int16_t tw = target.measureText(compactFont, label, smallStyle).width;
      const uint8_t point = static_cast<uint8_t>(j * 3);
      int16_t tx = static_cast<int16_t>(px(point) - tw / 2);
      if (tx < gx0) tx = gx0;
      if (tx > gx1 - tw) tx = static_cast<int16_t>(gx1 - tw);
      if (tx < gx0) tx = gx0;
      target.text(ui::Rect{tx, tickY, tw, smallLh}, label, smallStyle);
    }
  }
}

void renderDashboard(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  drawHeader(target, device, "Dashboard");

  constexpr int16_t kCardGap = 16;
  constexpr int16_t kCardHeight = 140;
  const int16_t top = static_cast<int16_t>(kMargin + target.lineHeight(ui::FONT_SLOT_BODY) + 20);
  const int16_t cardWidth = static_cast<int16_t>((device.width - 2 * kMargin - kCardGap) / 2);
  // 4.7: every card and row renders live from the AcService snapshot
  // ("no data" placeholder until the first state lands).
  const midea::AcState ac = acService.snapshot();
  const bool haveState = acService.hasState();
  // Stale: values on screen came from a session that is no longer alive.
  const bool stale = haveState && !acService.sessionUp();

  const ui::Rect targetCard{kMargin, top, cardWidth, kCardHeight};
  if (haveState) {
    const uint8_t halfSteps = tempHalfSteps(ac.targetTemperature);
    drawMetricCard(target, targetCard, "Target", halfSteps / 2, (halfSteps & 1) != 0);
  } else {
    drawMetricCardText(target, targetCard, "Target", "no data");
  }
  const ui::Rect indoorCard{static_cast<int16_t>(kMargin + cardWidth + kCardGap), top, cardWidth, kCardHeight};
  if (haveState && ac.indoorTemperature) {
    const double temp = *ac.indoorTemperature;
    const int degrees = static_cast<int>(temp >= 0.0 ? temp + 0.5 : temp - 0.5); // round, not truncate
    drawMetricCard(target, indoorCard, "Indoor", degrees);
  } else {
    drawMetricCardText(target, indoorCard, "Indoor", "no data");
  }

  // Group-7 real-time power draw (6.1d): scalar read; "no data" until the
  // first answer lands (the Dashboard's poll mode queries exactly this group).
  const std::optional<uint16_t> outdoorW = acService.outdoorUnitPowerW();
  char powerText[12];
  if (outdoorW) {
    snprintf(powerText, sizeof(powerText), "%u W", static_cast<unsigned>(*outdoorW));
  } else {
    snprintf(powerText, sizeof(powerText), "no data");
  }

  struct StatusRowSpec {
    const char* label;
    const char* value;
    bool snowflake;
  };
  char fanText[12];
  // Swing is a control detail, not a glance metric: it lives in Control only.
  // Power is folded into Mode — when the AC is off, "Off" is the state the
  // user cares about; when it is on, the active mode is more useful.
  const StatusRowSpec kRows[] = {
      {"Mode", haveState ? (ac.powerOn ? modeName(ac.operationalMode) : "Off") : "no data",
       haveState && ac.powerOn && ac.operationalMode == kModeCool},
      {"Fan", haveState ? fanName(ac.fanSpeed, fanText, sizeof(fanText)) : "no data", false},
      {"Draw", outdoorW ? powerText : "no data", false},
  };
  constexpr int16_t kRowHeight = 56;
  constexpr int16_t kRowGap = 12;
  constexpr uint8_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);
  const int16_t rowsTop = static_cast<int16_t>(top + kCardHeight + 24);
  // Landscape is only 480 px tall: rows can shrink first, then the weather
  // graph gives up its extra space before the whole panel disappears.
  const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);
  const int16_t wxLineLh = target.lineHeight(freedea::kFontSlotCompact);
  // Reserve a line for the stale indicator between the rows and the footer.
  const int16_t zoneBottom = static_cast<int16_t>(device.height - kMargin - smallLh - 8 - (stale ? smallLh + 6 : 0));
  int16_t rowsBottom = zoneBottom;

  // Weather panel (6.2c): stacks below the AC rows, hidden until configured
  // and fetched. The rows give up height first (down to 24 px), then the
  // forecast graph drops hour labels, then the graph itself, and only last
  // does the whole weather panel vanish — the AC rows keep a usable size.
  WeatherService::Snapshot wxSnap;
  const settings::Weather& wxCfg = settingsStore.settings().weather;
  bool wxPanel = wxCfg.enabled && settings::weatherConfigured(wxCfg) && weatherService.snapshot(wxSnap);
  bool wxSpark = false;
  bool wxTicks = false;
  int16_t wxPanelTop = 0;
  int16_t wxPanelH = 0;
  int16_t wxMinPanelH = 0;
  int16_t rowHeight = 0;
  const int16_t rowGaps = static_cast<int16_t>((kRowCount - 1) * kRowGap);
  constexpr int16_t kRowsPanelGap = 6;

  auto rowsFitWithPanel = [&](const int16_t panelH) {
    return static_cast<int16_t>((zoneBottom - rowsTop - rowGaps - panelH - kRowsPanelGap) / kRowCount);
  };

  if (wxPanel) {
    const bool haveHourly = wxSnap.data.hourlyCount == weather::kHourlyCount;
    wxSpark = haveHourly;
    wxTicks = haveHourly;
    wxMinPanelH = weatherPanelHeight(wxLineLh, wxLineLh, wxSpark, wxTicks);
    int16_t fitRows = rowsFitWithPanel(wxMinPanelH);
    if (fitRows < 24 && haveHourly) {
      wxTicks = false;
      wxMinPanelH = weatherPanelHeight(wxLineLh, wxLineLh, true, false);
      fitRows = rowsFitWithPanel(wxMinPanelH);
    }
    if (fitRows < 24) {
      wxSpark = false;
      wxMinPanelH = weatherPanelHeight(wxLineLh, wxLineLh, false, false);
      fitRows = rowsFitWithPanel(wxMinPanelH);
      if (fitRows < 24) {
        wxPanel = false; // no vertical room at all: rows win
      }
    }
  }

  if (wxPanel) {
    const int16_t fitRows = rowsFitWithPanel(wxMinPanelH);
    rowHeight = fitRows < kRowHeight ? fitRows : kRowHeight;
    if (rowHeight < 24) rowHeight = 24; // negotiation above guarantees the space
    rowsBottom = rowsTop + kRowCount * rowHeight + rowGaps;
    wxPanelTop = rowsBottom + kRowsPanelGap;
    // The graph is the point of the panel, so it absorbs the remaining vertical
    // space. Text-only weather stays compact instead of stretching a single
    // conditions line into a tall empty box.
    wxPanelH = wxSpark ? static_cast<int16_t>(zoneBottom - wxPanelTop) : wxMinPanelH;
    if (wxPanelH < wxMinPanelH) wxPanelH = wxMinPanelH;
  } else {
    const int16_t fit = static_cast<int16_t>((zoneBottom - rowsTop - rowGaps) / kRowCount);
    rowHeight = fit < kRowHeight ? fit : kRowHeight;
  }

  for (uint8_t i = 0; i < kRowCount; ++i) {
    const ui::Rect row{kMargin, static_cast<int16_t>(rowsTop + i * (rowHeight + kRowGap)),
                       static_cast<int16_t>(device.width - 2 * kMargin), rowHeight};
    drawStatusRow(target, row, kRows[i].label, kRows[i].value, kRows[i].snowflake);
  }

  if (wxPanel) {
    const ui::Rect panel{kMargin, wxPanelTop, static_cast<int16_t>(device.width - 2 * kMargin), wxPanelH};
    drawWeatherPanel(target, panel, wxSnap, wxSpark, wxTicks);
  }
  if (stale) {
    // The counter only ages while this screen repaints; the main loop ticks
    // it every ~10 s so it never sits frozen on a wrong value.
    char staleText[48];
    formatStaleText(staleText, sizeof(staleText));
    ui::TextStyle staleStyle{};
    staleStyle.font = ui::FONT_SLOT_SMALL;
    staleStyle.align = ui::TextAlign::Center;
    // Below the panel when it exists, otherwise directly under the rows.
    const int16_t staleY =
        wxPanel ? static_cast<int16_t>(wxPanelTop + wxPanelH + 4) : static_cast<int16_t>(rowsBottom + 6);
    target.text(ui::Rect{0, staleY, device.width, smallLh}, staleText, staleStyle);
  }

  setButtonHints({.back = "Back", .confirm = "Details"});
}

// Dashboard is passive except Confirm, which opens the group-data Details
// screen (6.1d); the poll-mode mapping picks up the switch from the screen id.
void dashboardButton(Navigation& nav, uint8_t button) {
  if (button == InputManager::BTN_CONFIRM) {
    nav.push(ScreenId::Details);
  } else if (button == InputManager::BTN_BACK) {
    nav.pop();
  }
}

// --- Details (6.1d) ----------------------------------------------------------

// Group-4 energy decode. midea-msmart prefers BCD whenever any BCD field is
// non-zero, but the Porti encodes binary: a plug-meter check (2026-09-07)
// showed the whole-unit draw matching group 7 (~135 W) and the binary G4
// decode (138.5 W), while the BCD decode of the same bytes (0x69 0x05 =
// 0x0569) misreads them as digits "0569" -> 56.9 W, about half the real
// draw. Keep the selector for devices that genuinely encode BCD.
constexpr bool kUseGroup4BinaryEnergy = true;

// Scroll offset into the Details rows; clamped to the renderable range in
// renderDetails (visible count) and bounded above by sDetailsMaxScroll so
// the button handler never scrolls past the last row.
uint8_t sDetailsScroll = 0;
uint8_t sDetailsMaxScroll = 0; // set by renderDetails: last reachable offset

// One-decimal fixed-point ASCII ("18.0 C", "56.9 W"). Integer-formatted on
// purpose: soft-float %f drags weight into the draw path. Returns nullptr for
// "no value", letting the caller show the placeholder.
const char* fmtTenth(char* buf, size_t cap, const std::optional<double>& value, const char* suffix) {
  if (!value) {
    return nullptr;
  }
  const int32_t tenths = static_cast<int32_t>(*value * 10.0 + (*value >= 0.0 ? 0.5 : -0.5));
  const uint32_t frac = static_cast<uint32_t>(tenths >= 0 ? tenths % 10 : -(tenths % 10));
  snprintf(buf, cap, "%d.%u%s", static_cast<int>(tenths / 10), frac, suffix);
  return buf;
}

// Energy in hundredths of a kWh ("0.50 kWh").
const char* fmtKwh(char* buf, size_t cap, const std::optional<double>& value) {
  if (!value) {
    return nullptr;
  }
  const uint32_t hundredths = static_cast<uint32_t>(*value * 100.0 + 0.5);
  snprintf(buf, cap, "%u.%02u kWh", static_cast<unsigned>(hundredths / 100), static_cast<unsigned>(hundredths % 100));
  return buf;
}

struct DetailRow {
  const char* label;
  char value[16];
};
constexpr uint8_t kDetailRowCount = 15;

void renderDetails(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  drawHeader(target, device, "Details");

  // The ~160 B snapshot and the row table together would blow the draw-path
  // stack budget; both are function-static scratch (rendering only ever runs
  // on the main loop, no re-entry).
  static midea::AcExtStats ext;
  static DetailRow rows[kDetailRowCount];
  ext = acService.extSnapshot();
  const uint16_t seen = acService.extGroupsSeen();
  // No per-row seen-mask checks needed: a group that never answered leaves
  // its fields nullopt, which renders as "no data".

  // Fixed 15-row table; kDetailRowCount must equal the number of add calls.
  constexpr size_t n = kDetailRowCount;
  DetailRow* row = rows;
  const auto addText = [&](const char* label, const char* value) {
    row->label = label;
    snprintf(row->value, sizeof(row->value), "%s", value);
    ++row;
  };
  const auto addNum = [&](const char* label, const std::optional<double>& value, const char* suffix) {
    row->label = label;
    if (!fmtTenth(row->value, sizeof(row->value), value, suffix)) {
      snprintf(row->value, sizeof(row->value), "no data");
    }
    ++row;
  };
  const auto addKwh = [&](const char* label, const std::optional<double>& value) {
    row->label = label;
    if (!fmtKwh(row->value, sizeof(row->value), value)) {
      snprintf(row->value, sizeof(row->value), "no data");
    }
    ++row;
  };
  // Integer-formatted optional (RPM, volts, raw bytes).
  const auto addCount = [&](const char* label, const auto& value, const char* fmt) {
    row->label = label;
    if (value) {
      snprintf(row->value, sizeof(row->value), fmt, static_cast<unsigned>(*value));
    } else {
      snprintf(row->value, sizeof(row->value), "no data");
    }
    ++row;
  };

  // Compressor: "act / target Hz", or just the actual if the target is unset.
  if (ext.compressorFrequencyHz) {
    char hz[16];
    if (ext.compressorTargetFrequencyHz) {
      snprintf(hz, sizeof(hz), "%u / %u Hz", static_cast<unsigned>(*ext.compressorFrequencyHz),
               static_cast<unsigned>(*ext.compressorTargetFrequencyHz));
    } else {
      snprintf(hz, sizeof(hz), "%u Hz", static_cast<unsigned>(*ext.compressorFrequencyHz));
    }
    addText("Compressor", hz);
  } else {
    addText("Compressor", "no data");
  }
  // Compressor current: unitless raw byte — midea-msmart leaves the unit
  // undefined upstream, so no unit is asserted here either.
  addCount("Current (raw)", ext.compressorCurrent, "%u");
  addCount("Voltage", ext.compressorVoltageV, "%u V");
  addNum("Indoor coil", ext.t2IndoorCoilC, " C");
  addNum("Outdoor coil", ext.t3OutdoorCoilC, " C");
  addNum("Outdoor temp", ext.t4OutdoorAmbientC, " C");
  addCount("Discharge", ext.dischargePipeC, "%u C");
  addCount("Indoor fan", ext.indoorFanRpm, "%u RPM");
  addCount("Fan target", ext.indoorFanTargetRpm, "%u RPM");
  addCount("Outdoor fan", ext.outdoorFanRpm, "%u RPM");
  // Humidity: the group-5 0 is the no-sensor sentinel (parser keeps nullopt).
  addCount("Humidity", ext.humidityPercent, "%u %%");
  addCount("Outdoor power", ext.outdoorUnitPowerW, "%u W");
  addNum("Power now", kUseGroup4BinaryEnergy ? ext.realTimePowerWBinary : ext.realTimePowerWBcd, " W");
  addKwh("Energy (run)", kUseGroup4BinaryEnergy ? ext.runEnergyKwhBinary : ext.runEnergyKwhBcd);
  addKwh("Energy (total)", kUseGroup4BinaryEnergy ? ext.totalEnergyKwhBinary : ext.totalEnergyKwhBcd);

  // Layout: fixed-height rows from under the header to the hint lines; the
  // scroll offset clamps against what actually fits (orientation-aware).
  constexpr int16_t kRowGap = 12;
  constexpr int16_t kRowHeight = 56;
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
  const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);
  const bool stale = seen != 0 && !acService.sessionUp();
  const int16_t hardBottom = static_cast<int16_t>(device.height - kMargin - smallLh - 8);
  // Reserve hint lines first (stale, plus the scroll position when needed).
  const int16_t fitAll = (hardBottom - (kMargin + lh + 20) + kRowGap) / (kRowHeight + kRowGap);
  const bool scrollable = fitAll < static_cast<int16_t>(n);
  const int16_t rowsBottom =
      static_cast<int16_t>(hardBottom - (scrollable ? smallLh + 6 : 0) - (stale ? smallLh + 6 : 0));
  const int16_t rowsTop = static_cast<int16_t>(kMargin + lh + 20);
  int32_t visible = (rowsBottom - rowsTop + kRowGap) / (kRowHeight + kRowGap);
  if (visible < 1) {
    visible = 1;
  }
  const uint8_t visibleRows = static_cast<uint8_t>(visible > static_cast<int32_t>(n) ? n : visible);
  sDetailsMaxScroll = static_cast<uint8_t>(n > visibleRows ? n - visibleRows : 0);
  if (sDetailsScroll > sDetailsMaxScroll) {
    sDetailsScroll = sDetailsMaxScroll;
  }
  for (uint8_t i = 0; i < visibleRows; ++i) {
    const DetailRow& r = rows[sDetailsScroll + i];
    const ui::Rect rowRect{kMargin, static_cast<int16_t>(rowsTop + i * (kRowHeight + kRowGap)),
                           static_cast<int16_t>(device.width - 2 * kMargin), kRowHeight};
    drawStatusRow(target, rowRect, r.label, r.value, false);
  }

  ui::TextStyle hintText{};
  hintText.font = ui::FONT_SLOT_SMALL;
  hintText.align = ui::TextAlign::Center;
  int16_t hintY = rowsBottom + 6;
  if (scrollable) {
    char pos[24];
    snprintf(pos, sizeof(pos), "rows %u-%u of %u", static_cast<unsigned>(sDetailsScroll + 1),
             static_cast<unsigned>(sDetailsScroll + visibleRows), static_cast<unsigned>(n));
    target.text(ui::Rect{0, hintY, device.width, smallLh}, pos, hintText);
    hintY = static_cast<int16_t>(hintY + smallLh + 6);
  }
  if (stale) {
    // Reconnect attempts continue (Details keeps the service in kStateAll);
    // the line only says how old the shown group data is.
    const uint32_t ageS = acService.lastExtAgeMs() / 1000;
    char staleText[48];
    if (ageS >= 3600) {
      snprintf(staleText, sizeof(staleText), "stale - last data %u h ago", static_cast<unsigned>(ageS / 3600));
    } else {
      snprintf(staleText, sizeof(staleText), "stale - last data %u:%02u ago", static_cast<unsigned>(ageS / 60),
               static_cast<unsigned>(ageS % 60));
    }
    target.text(ui::Rect{0, hintY, device.width, smallLh}, staleText, hintText);
  }

  setButtonHints({.back = "Back", .up = "Up", .down = "Down"});
}

void detailsButton(Navigation& nav, uint8_t button) {
  aliasSideToNav(button);
  switch (button) {
    case InputManager::BTN_UP:
      if (sDetailsScroll > 0) {
        --sDetailsScroll;
        nav.requestRender();
      }
      break;
    case InputManager::BTN_DOWN:
      if (sDetailsScroll < sDetailsMaxScroll) {
        ++sDetailsScroll;
        nav.requestRender();
      }
      break;
    case InputManager::BTN_BACK:
      nav.pop();
      break;
    default:
      break;
  }
}

// --- Control --------------------------------------------------------------------

// Rows list: Up/Down move focus, Left/Right quick-adjust the focused row
// (enum rows wrap, temperature steps 0.5 °C and clamps, toggles flip).
// Confirm opens the row's options as a modal dropdown pre-positioned on the
// current value: Up/Down move, Confirm saves, Back cancels. Outside the
// dropdown, Back returns. Saving composes the value into sControlDesired and
// queues a SetState; rows keep displaying the device's acknowledged snapshot
// until the service's post-send GetState lands. With power off (per the
// snapshot), the temp/fan/swing rows are inert (temp shows "--", others
// render their value in parentheses). Before the first state arrives every
// row shows "no data" and edits are ignored: there is no truth to compose a
// command from. The property-channel rows (8.3: iECO, out-silent; 8.5: self
// clean, breeze away, breezeless, jet cool; 8.6: power limit, wind around,
// fresh air) skip the composition entirely: they display the service's
// acknowledged property snapshot and queue kSetProperty commands. The four SetState-bit rows
// (8.4: ionizer, 8 °C heat, sleep, follow me) ride the Eco/Turbo path:
// composed into sControlDesired, rendered from the AcState snapshot. The
// display row (8.7) is a state-backed action row: Confirm sends the
// toggle-only command and the row follows AcState::displayOn. The sound row
// (8.8) is setting-backed — the device cannot report the buzzer, so it
// displayed the persisted Sound setting (a documented exception to
// acknowledged state), works without a session, and queues a BUZZER write
// plus a settings save on change. The button-lock row (8.9) is likewise
// setting-backed: the lock lives in the service (it holds a session, polls,
// and overrides external changes), so the row only flips the persisted flag.
constexpr uint8_t kControlItemCount = 22;
constexpr uint8_t kMaxControlOptions = 16; // max list span (temp 17..30 whole = 14)
// "8C heat" without the degree mark: the bundled font covers U+0020–007E only.
constexpr const char* kControlLabels[kControlItemCount] = {
    "Mode",        "Target",      "Fan",       "Swing",     "Eco",        "Turbo",       "iECO",       "Out silent",
    "Ionizer",     "8C heat",     "Sleep",     "Follow me", "Self clean", "Breeze away", "Breezeless", "Jet cool",
    "Power limit", "Wind around", "Fresh air", "Display",   "Sound",      "Button lock"};

uint8_t sControlFocus = 0;  // slot among the visible rows
uint8_t sControlScroll = 0; // first drawn slot when the rows overflow the band (8.4)
bool sDropdownOpen = false;
uint8_t sDropdownSel = 0;

// --- Capability gating (4.5) -------------------------------------------------
// Options and row visibility come from the AcService capability snapshot.
// Tri-state semantics: only an explicit kFalse hides an option — kAbsent (the
// device never reported the key) stays permissive, as does everything before
// the first snapshot (then the full set shows).
struct CapView {
  midea::AcCapabilities caps;
  AcService::Props props; // acknowledged property-channel snapshot (8.3)
  bool known = false;
  bool supports(midea::CapFlag flag) const { return !known || flag != midea::CapFlag::kFalse; }
};

CapView controlCaps() {
  CapView view;
  view.props = acService.props();
  view.known = acService.hasCapabilities();
  if (view.known) {
    view.caps = acService.capabilities();
  }
  return view;
}

// Option list for one row under the current capabilities: raw Mock enum
// values (the temperature row holds temperatures directly).
struct ControlOptions {
  uint8_t values[kMaxControlOptions];
  uint8_t count = 0;
};

bool controlRowDisabled(uint8_t row, bool powerOff) {
  return powerOff && (row == 1 || row == 2 || row == 3);
}

bool controlRowAvailable(uint8_t row, const CapView& view) {
  // Property-channel rows (8.3) gate inversely to the SetState rows: they
  // appear only on an explicit kTrue capability AND once the property GET
  // answered with a value. No caps or an unanswered GET keeps them hidden —
  // there is no acknowledged truth to show and a denied set would error.
  if (row == 6) return view.known && view.caps.ieco == midea::CapFlag::kTrue && view.props.iecoKnown;
  if (row == 7) return view.known && view.caps.outSilent == midea::CapFlag::kTrue && view.props.outSilentKnown;
  if (row == 12) return view.known && view.caps.selfClean == midea::CapFlag::kTrue && view.props.selfCleanKnown;
  if (row == 13) return view.known && view.caps.breezeAway == midea::CapFlag::kTrue && view.props.breezeAwayKnown;
  if (row == 14) return view.known && view.caps.breezeless == midea::CapFlag::kTrue && view.props.breezelessKnown;
  if (row == 15) return view.known && view.caps.flash == midea::CapFlag::kTrue && view.props.flashKnown;
  if (row == 16)
    return view.known &&
           (view.caps.rateSelect2Level == midea::CapFlag::kTrue ||
            view.caps.rateSelect5Level == midea::CapFlag::kTrue) &&
           view.props.rateSelectKnown;
  if (row == 17) return view.known && view.caps.cascade == midea::CapFlag::kTrue && view.props.cascadeKnown;
  if (row == 18) return view.known && view.caps.freshAir == midea::CapFlag::kTrue && view.props.freshAirKnown;
  // Display (8.7): state-backed (AcState::displayOn), so no property-known
  // gate — only the caps flag decides whether the toggle is accepted.
  if (row == 19) return view.known && view.caps.displayControl == midea::CapFlag::kTrue;
  // Sound (8.8): setting-backed and never caps-gated (msmart parses the
  // BUZZER capability but gates nothing) — falls through to always shown.
  // Capability-hidable rows: swing disappears when neither direction works,
  // eco when the device denies it, turbo when both per-direction turbo flags
  // deny it (tri-state: only explicit kFalse hides).
  if (!view.known) return true;
  switch (row) {
    case 3:
      return view.caps.swingHorizontal != midea::CapFlag::kFalse || view.caps.swingVertical != midea::CapFlag::kFalse;
    case 4:
      return view.caps.eco != midea::CapFlag::kFalse;
    case 5:
      return view.caps.turboHeat != midea::CapFlag::kFalse || view.caps.turboCool != midea::CapFlag::kFalse;
    // SetState-bit feature rows (8.4): ionizer and 8C heat hide only on an
    // explicit capability denial; sleep and follow me have no capability in
    // msmart, so they are always shown.
    case 8:
      return view.caps.anion != midea::CapFlag::kFalse;
    case 9:
      return view.caps.freezeProtection != midea::CapFlag::kFalse;
    default:
      return true;
  }
}

uint8_t controlVisibleRowCount(const CapView& view) {
  uint8_t n = 0;
  for (uint8_t row = 0; row < kControlItemCount; ++row) {
    if (controlRowAvailable(row, view)) ++n;
  }
  return n;
}

uint8_t controlVisibleRowAt(uint8_t slot, const CapView& view) {
  for (uint8_t row = 0; row < kControlItemCount; ++row) {
    if (controlRowAvailable(row, view) && slot-- == 0) return row;
  }
  return 0;
}

// Target range for a mode when the device reported temperature ranges
// (0x0225); intersected with the supported span, falling back to it whole.
void controlTempRange(const CapView& view, uint8_t mode, uint8_t& lo, uint8_t& hi) {
  lo = kTempMinC;
  hi = kTempMaxC;
  if (!view.known || !view.caps.hasTemperatureRanges) return;
  double mn = 0.0;
  double mx = 0.0;
  switch (mode) {
    case kModeCool:
      mn = view.caps.coolMin;
      mx = view.caps.coolMax;
      break;
    case kModeHeat:
      mn = view.caps.heatMin;
      mx = view.caps.heatMax;
      break;
    default:
      mn = view.caps.autoMin;
      mx = view.caps.autoMax;
      if (mn <= 0.0 || mx <= mn) {
        mn = view.caps.coolMin;
        mx = view.caps.coolMax;
      }
      break;
  }
  if (mn <= 0.0 || mx <= mn) return;
  const int loR = static_cast<int>(mn + 0.5);
  const int hiR = static_cast<int>(mx + 0.5);
  const int loC = loR < kTempMinC ? kTempMinC : loR;
  const int hiC = hiR > kTempMaxC ? kTempMaxC : hiR;
  if (loC <= hiC) {
    lo = static_cast<uint8_t>(loC);
    hi = static_cast<uint8_t>(hiC);
  }
}

// Option raw values are protocol-native (mode number, half-degrees, fan
// percent, swing nibble, toggle 0/1) so sending needs no re-mapping.
void buildControlOptions(uint8_t row, const CapView& view, ControlOptions& out) {
  out.count = 0;
  switch (row) {
    case 0: {
      struct ModeOption {
        uint8_t mode;
        midea::CapFlag flag;
      };
      // Off (clears power) plus the capability-supported modes; Off and
      // fan-only have no capability flag and always stay.
      const ModeOption modes[] = {
          {kModeOffOption, midea::CapFlag::kTrue}, {kModeAuto, view.caps.autoMode}, {kModeCool, view.caps.coolMode},
          {kModeDry, view.caps.dryMode},           {kModeHeat, view.caps.heatMode}, {kModeFan, midea::CapFlag::kTrue},
      };
      for (const ModeOption& entry : modes) {
        if (view.supports(entry.flag)) out.values[out.count++] = entry.mode;
      }
      break;
    }
    case 1: {
      uint8_t lo = 0;
      uint8_t hi = 0;
      controlTempRange(view, sControlDesired.operationalMode, lo, hi);
      // Whole degrees in the dropdown (raw values are half-degrees, hence
      // the *2 bounds and step-2 fill); Left/Right step 0.5 °C.
      for (uint8_t t = static_cast<uint8_t>(lo * 2);
           t <= static_cast<uint8_t>(hi * 2) && out.count < kMaxControlOptions; t += 2) {
        out.values[out.count++] = t;
      }
      break;
    }
    case 2: {
      struct FanOption {
        uint8_t percent;
        midea::CapFlag flag;
      };
      // Named fan speeds are sent as the protocol percent (msmart FanSpeed).
      // Max (100 %) gates on fanCustom but is msmart's "additional" speed on
      // top of the named list — it never replaces it: devices reporting only
      // custom speeds (0x0210 value 1) still accept the named percents, which
      // is why msmart skips its unsupported-speed warning for them too.
      const FanOption fans[] = {
          {midea::kFanSpeedAuto, view.caps.fanAuto}, {kFanSilent, view.caps.fanSilent}, {kFanLow, view.caps.fanLow},
          {kFanMedium, view.caps.fanMedium},         {kFanHigh, view.caps.fanHigh},
      };
      uint8_t kept = 0;
      for (const FanOption& entry : fans) {
        if (view.supports(entry.flag)) out.values[kept++] = entry.percent;
      }
      if (kept == 0) {
        // All named flags false is no usable information (custom-only or
        // unknown flag sets): keep the full named set.
        for (const FanOption& entry : fans)
          out.values[kept++] = entry.percent;
      }
      if (view.supports(view.caps.fanCustom) && kept < kMaxControlOptions) {
        out.values[kept++] = kFanMax;
      }
      out.count = kept;
      break;
    }
    case 3: {
      // Off always; Vertical/Horizontal per direction; Both only when both
      // directions work.
      const bool vert = view.supports(view.caps.swingVertical);
      const bool horz = view.supports(view.caps.swingHorizontal);
      out.values[out.count++] = kSwingOff;
      if (vert) out.values[out.count++] = kSwingVertical;
      if (horz) out.values[out.count++] = kSwingHorizontal;
      if (vert && horz) out.values[out.count++] = kSwingBoth;
      break;
    }
    case 16: {
      // Rate-select gears are protocol bytes: 100 off plus the caps-derived
      // level set; 5-level wins when both flags report, as in msmart's
      // rate_select_levels().
      out.values[out.count++] = 100;
      if (view.caps.rateSelect5Level == midea::CapFlag::kTrue) {
        static constexpr uint8_t kGears[] = {1, 20, 40, 60, 80};
        for (const uint8_t gear : kGears)
          out.values[out.count++] = gear;
      } else if (view.caps.rateSelect2Level == midea::CapFlag::kTrue) {
        out.values[out.count++] = 50;
        out.values[out.count++] = 75;
      }
      break;
    }
    case 17: {
      static constexpr uint8_t kCascadeModes[] = {0, 1, 2}; // Off/Up/Down
      for (const uint8_t mode : kCascadeModes)
        out.values[out.count++] = mode;
      break;
    }
    case 18: {
      static constexpr uint8_t kFreshAirSpeeds[] = {0, 40, 60, 80, 100};
      for (const uint8_t speed : kFreshAirSpeeds)
        out.values[out.count++] = speed;
      break;
    }
    default: {
      // On/Off toggles (Eco, Turbo, iECO, out-silent, ionizer, 8C heat,
      // sleep, follow me).
      out.values[out.count++] = 0;
      out.values[out.count++] = 1;
      break;
    }
  }
}

// Current value of a row from the composed desired state: dropdown
// positioning and stepping track pending edits, not the (lagging) snapshot.
uint8_t controlRawValue(uint8_t row) {
  switch (row) {
    case 0:
      return sControlDesired.powerOn ? sControlDesired.operationalMode : kModeOffOption;
    case 1:
      return tempHalfSteps(sControlDesired.targetTemperature);
    case 2:
      return sControlDesired.fanSpeed;
    case 3:
      return sControlDesired.swingMode;
    case 4:
      return sControlDesired.eco ? 1 : 0;
    case 5:
      return sControlDesired.turbo ? 1 : 0;
    case 6:
      return acService.props().ieco ? 1 : 0;
    case 7:
      return acService.props().outSilent ? 1 : 0;
    case 8:
      return sControlDesired.purifier ? 1 : 0;
    case 9:
      return sControlDesired.freezeProtection ? 1 : 0;
    case 10:
      return sControlDesired.sleep ? 1 : 0;
    case 11:
      return sControlDesired.followMe ? 1 : 0;
    case 12:
      return 0; // trigger row: never stepped or shown in a dropdown
    case 13:
      return acService.props().breezeAway ? 1 : 0;
    case 14:
      return acService.props().breezeless ? 1 : 0;
    case 15:
      return acService.props().flash ? 1 : 0;
    case 16:
      return acService.props().rateSelect;
    case 17:
      return acService.props().cascade;
    case 18:
      return acService.props().freshAir;
    case 20:
      return settingsStore.settings().ac.beep ? 1 : 0; // setting-backed (8.8)
    case 21:
      return settingsStore.settings().ac.buttonLock != 0 ? 1 : 0; // setting-backed (8.9)
    default:
      return 0; // row 19: action row, never stepped or shown in a dropdown
  }
}

// Row -> property-channel queue tag (8.3/8.5); false for SetState rows. Row
// 12 (self clean) is a one-shot trigger: only ever sent "on".
bool controlPropKind(uint8_t row, AcService::PropKind& kind) {
  switch (row) {
    case 6:
      kind = AcService::PropKind::kIeco;
      return true;
    case 7:
      kind = AcService::PropKind::kOutSilent;
      return true;
    case 12:
      kind = AcService::PropKind::kSelfClean;
      return true;
    case 13:
      kind = AcService::PropKind::kBreezeAway;
      return true;
    case 14:
      kind = AcService::PropKind::kBreezeless;
      return true;
    case 15:
      kind = AcService::PropKind::kFlash;
      return true;
    case 16:
      kind = AcService::PropKind::kRateSelect;
      return true;
    case 17:
      kind = AcService::PropKind::kCascade;
      return true;
    case 18:
      kind = AcService::PropKind::kFreshAir;
      return true;
    default:
      return false;
  }
}

// Compose an edit into the desired state and queue it for the service. Before
// the first state there is nothing to compose the unchanged fields from, so
// edits are ignored; the queue-full drop is logged by the service.
void applyControlValue(uint8_t row, uint8_t raw) {
  // Sound (8.8) edits the persisted setting, not the AC: the buzzer is not
  // readable back, so the row shows the setting (documented exception to
  // acknowledged state) and needs no session — the queued BUZZER write rides
  // the next property send, and every command's beep bit follows the setting
  // in the service already. A failed queue only delays the device-side chirp
  // setting; the setting itself (and future commands' beep bit) still stuck.
  if (row == 20) {
    settings::Settings& s = settingsStore.settings();
    const bool on = raw != 0;
    if (s.ac.beep == on) return;
    s.ac.beep = on;
    if (!settingsStore.saveNow()) showToast("Save failed - not persisted");
    AcService::Command command;
    command.kind = AcService::Command::Kind::kSetProperty;
    command.propKind = AcService::PropKind::kBeep;
    command.propValue = on ? 1 : 0;
    if (!acService.queueCommand(command)) {
      Serial.println("[ui] command queue full, beep edit dropped");
      showToast("Command queue full");
    }
    return;
  }
  // Button lock (8.9) flips only the persisted flag: the service task reads
  // it live, arms on the next published state, and holds the session polling
  // while enabled — no AC command carries it. The toast names the cost of
  // the always-connected mode the lock opts into.
  if (row == 21) {
    settings::Settings& s = settingsStore.settings();
    const bool on = raw != 0;
    if (s.ac.buttonLock == on) return;
    s.ac.buttonLock = on;
    if (!settingsStore.saveNow()) showToast("Save failed - not persisted");
    if (on) showToast("Lock: always-polling + auto-override");
    return;
  }
  if (!acService.hasState()) return;
  // Property-channel rows (8.3/8.5) bypass the desired-state composition
  // entirely: they ride the 0xB0 queue entry, and the row keeps displaying the
  // last acknowledged property value until the service's post-set re-query lands.
  AcService::PropKind propKind;
  if (controlPropKind(row, propKind)) {
    if (raw == 0 && row == 12) return; // self clean has no off command
    AcService::Command command;
    command.kind = AcService::Command::Kind::kSetProperty;
    command.propKind = propKind;
    // Enum rows (8.6) carry the protocol value byte through; boolean rows
    // squash to 0/1.
    const bool enumRow = propKind == AcService::PropKind::kRateSelect || propKind == AcService::PropKind::kCascade ||
                         propKind == AcService::PropKind::kFreshAir;
    command.propValue = enumRow ? raw : static_cast<uint8_t>(raw != 0 ? 1 : 0);
    if (!acService.queueCommand(command)) {
      Serial.println("[ui] command queue full, prop edit dropped");
      showToast("Command queue full");
    }
    return;
  }
  switch (row) {
    case 0:
      if (raw == kModeOffOption) {
        sControlDesired.powerOn = false;
      } else {
        sControlDesired.powerOn = true;
        sControlDesired.operationalMode = raw;
      }
      break;
    case 1:
      sControlDesired.targetTemperature = tempFromHalfSteps(raw);
      break;
    case 2:
      sControlDesired.fanSpeed = raw;
      break;
    case 3:
      sControlDesired.swingMode = raw;
      break;
    case 4:
      sControlDesired.eco = raw != 0;
      break;
    case 5:
      sControlDesired.turbo = raw != 0;
      break;
    case 8:
      sControlDesired.purifier = raw != 0;
      break;
    case 9:
      sControlDesired.freezeProtection = raw != 0;
      break;
    case 10:
      sControlDesired.sleep = raw != 0;
      break;
    default:
      sControlDesired.followMe = raw != 0;
      break;
  }
  AcService::Command command;
  command.kind = AcService::Command::Kind::kSetState;
  command.desired = sControlDesired;
  if (!acService.queueCommand(command)) {
    // Full 4-deep queue means the service task is wedged or the user is
    // faster than the link; say so instead of dropping silently.
    Serial.println("[ui] command queue full, edit dropped");
    showToast("Command queue full");
  }
}

// Position of the current value inside the option list (0 when the value is
// currently outside it, e.g. a mode filtered in later).
uint8_t controlValueIndex(uint8_t row, const ControlOptions& opts) {
  const uint8_t raw = controlRawValue(row);
  for (uint8_t i = 0; i < opts.count; ++i) {
    if (opts.values[i] == raw) return i;
  }
  return 0;
}

// 8.6 enum value names. The 2-level rate-select gears read as percents; the
// 5-level ones are msmart's LEVEL_n gears (value 1 is not a percentage).
const char* rateSelectName(uint8_t value, char* buf, size_t bufSize) {
  switch (value) {
    case 100:
      return "Off";
    case 50:
      return "50%";
    case 75:
      return "75%";
    case 1:
      return "Level 1";
    case 20:
      return "Level 2";
    case 40:
      return "Level 3";
    case 60:
      return "Level 4";
    case 80:
      return "Level 5";
    default:
      snprintf(buf, bufSize, "%u", static_cast<unsigned>(value));
      return buf;
  }
}

const char* freshAirName(uint8_t speed, char* buf, size_t bufSize) {
  switch (speed) {
    case 0:
      return "Off";
    case 40:
      return "Low";
    case 60:
      return "Medium";
    case 80:
      return "High";
    case 100:
      return "Boost";
    default:
      snprintf(buf, bufSize, "%u", static_cast<unsigned>(speed));
      return buf;
  }
}

// Option label for dropdown row items; temp formats into buf and returns it.
const char* controlOptionName(uint8_t row, uint8_t value, char* buf, size_t bufSize) {
  switch (row) {
    case 0:
      return value == kModeOffOption ? "Off" : modeName(value);
    case 1:
      if (value & 1) {
        snprintf(buf, bufSize, "%d.5", static_cast<int>(value / 2));
      } else {
        snprintf(buf, bufSize, "%d", static_cast<int>(value));
      }
      return buf;
    case 2:
      return fanName(value, buf, bufSize);
    case 3:
      return swingName(value);
    case 16:
      return rateSelectName(value, buf, bufSize);
    case 17:
      return value == 1 ? "Up" : value == 2 ? "Down" : "Off";
    case 18:
      return freshAirName(value, buf, bufSize);
    default:
      return value ? "On" : "Off";
  }
}

bool adjustControl(int32_t delta, const CapView& view, bool powerOff) {
  const uint8_t row = controlVisibleRowAt(sControlFocus, view);
  // The sound row edits a setting, not the AC, so it steps without a state.
  if (controlRowDisabled(row, powerOff) || (row != 20 && !acService.hasState())) return false;
  if (row == 12 || row == 19) return false; // self clean and display are Confirm-only rows
  if (row == 1) {
    // Temperature steps 0.5 °C over the capability range, clamped; the
    // dropdown only lists whole degrees, so stepping is not index-based.
    uint8_t lo = 0;
    uint8_t hi = 0;
    controlTempRange(view, sControlDesired.operationalMode, lo, hi);
    int cur = tempHalfSteps(sControlDesired.targetTemperature);
    if (cur < static_cast<int>(lo) * 2) cur = static_cast<int>(lo) * 2;
    if (cur > static_cast<int>(hi) * 2) cur = static_cast<int>(hi) * 2;
    const int next = constrain(cur + delta, static_cast<int>(lo) * 2, static_cast<int>(hi) * 2);
    if (next == cur) return false;
    applyControlValue(row, static_cast<uint8_t>(next));
    return true;
  }
  ControlOptions opts;
  buildControlOptions(row, view, opts);
  const uint8_t idx = controlValueIndex(row, opts);
  const uint8_t next = cycleOption(static_cast<int32_t>(idx), delta, opts.count);
  if (opts.values[next] == controlRawValue(row)) return false;
  applyControlValue(row, opts.values[next]);
  return true;
}

// Dropdown layout: single source of truth for the full draw and the
// highlight blit, so the blit stays pixel-identical to a re-render.
struct DropdownLayout {
  ui::Rect panel;
  int16_t startY = 0;
  int16_t itemH = 0;
};

constexpr int16_t kDropdownItemGap = 6;
constexpr int16_t kDropdownPad = 12;

DropdownLayout dropdownLayout(ui::DisplayTarget& target, const ui::DeviceContext& device, uint8_t count) {
  const int16_t top = static_cast<int16_t>(kMargin + target.lineHeight(ui::FONT_SLOT_BODY) + 20);
  const int16_t bottom = static_cast<int16_t>(device.height - kMargin - 2 * target.lineHeight(ui::FONT_SLOT_SMALL));
  const ui::Rect panel{kMargin, top, static_cast<int16_t>(device.width - 2 * kMargin),
                       static_cast<int16_t>(bottom - top)};
  const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);
  const int16_t listTop = static_cast<int16_t>(panel.y + smallLh + 12 + kDropdownPad);
  const int16_t areaH = static_cast<int16_t>(panel.y + panel.height - kDropdownPad - listTop);
  int16_t itemH = static_cast<int16_t>((areaH - (count - 1) * kDropdownItemGap) / count);
  if (itemH > 56) itemH = 56;
  const int16_t listH = static_cast<int16_t>(count * itemH + (count - 1) * kDropdownItemGap);
  const int16_t startY = static_cast<int16_t>(listTop + (areaH - listH) / 2);
  return {panel, startY, itemH};
}

ui::Rect dropdownItemRect(const DropdownLayout& layout, uint8_t index) {
  return ui::Rect{static_cast<int16_t>(layout.panel.x + 12),
                  static_cast<int16_t>(layout.startY + index * (layout.itemH + kDropdownItemGap)),
                  static_cast<int16_t>(layout.panel.width - 24), layout.itemH};
}

// One option row. clear=true first restores the white panel behind it (the
// blit erasing the old selection); sel paints the inverted selection fill.
void drawControlItem(ui::DisplayTarget& target, const ui::Rect& item, uint8_t row, uint8_t value, bool sel,
                     bool clear) {
  if (clear) target.fill(item, ui::Paint::solid(ui::Color::White), 6);
  if (sel) target.fill(item, ui::Paint::solid(ui::Color::Black), 6);
  if (row == 1) {
    drawTemp(target, item, value / 2, true, sel, (value & 1) != 0);
    return;
  }
  char nameBuf[20];
  const char* name = controlOptionName(row, value, nameBuf, sizeof(nameBuf));
  ui::TextStyle itemStyle{};
  itemStyle.font = ui::FONT_SLOT_BODY;
  itemStyle.align = ui::TextAlign::Center;
  itemStyle.inverted = sel;
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
  target.text(ui::Rect{item.x, static_cast<int16_t>(item.y + (item.height - lh) / 2), item.width, lh}, name, itemStyle);
}

void controlButton(Navigation& nav, uint8_t button) {
  const CapView view = controlCaps();
  const uint8_t rowCount = controlVisibleRowCount(view);
  // Capabilities may have shrunk the visible rows (or an option list) since
  // the last draw; clamp stale cursor state before using it.
  if (sControlFocus >= rowCount) sControlFocus = 0;
  const uint8_t row = controlVisibleRowAt(sControlFocus, view);
  // "Off" disables rows from the acknowledged snapshot; before the first
  // state nothing is editable (applyControlValue guards the sends too).
  const midea::AcState shown = acService.snapshot();
  const bool haveState = acService.hasState();
  const bool powerOff = haveState && !shown.powerOn;

  if (sDropdownOpen) {
    ControlOptions opts;
    buildControlOptions(row, view, opts);
    if (sDropdownSel >= opts.count) sDropdownSel = 0;
    switch (button) {
      case InputManager::BTN_UP:
      case InputManager::BTN_DOWN: {
        const uint8_t prevSel = sDropdownSel;
        sDropdownSel =
            static_cast<uint8_t>((sDropdownSel + (button == InputManager::BTN_UP ? opts.count - 1 : 1)) % opts.count);
        if (sDropdownSel == prevSel) break; // single-option list: nothing moves
        // Stepping redraws only the two touched option boxes into the held
        // framebuffer and presents it directly: a full Control+panel rasterize
        // measured ~1.25 s, so stepping costs only the waveform.
        ui::DisplayTarget target = nav.heldFrame();
        const DropdownLayout layout = dropdownLayout(target, target.deviceContext(), opts.count);
        const ui::Rect oldItem = dropdownItemRect(layout, prevSel);
        drawControlItem(target, oldItem, row, opts.values[prevSel], false, true);
        const ui::Rect newItem = dropdownItemRect(layout, sDropdownSel);
        drawControlItem(target, newItem, row, opts.values[sDropdownSel], true, false);
        nav.presentHeldFrame();
        break;
      }
      case InputManager::BTN_CONFIRM:
        applyControlValue(row, opts.values[sDropdownSel]);
        sDropdownOpen = false;
        nav.requestRender();
        break;
      case InputManager::BTN_BACK:
        sDropdownOpen = false; // cancel without saving
        nav.requestRender();
        break;
      default:
        break; // Left/Right are ignored while the modal is open
    }
    return;
  }

  switch (button) {
    case InputManager::BTN_UP:
      sControlFocus = static_cast<uint8_t>((sControlFocus + rowCount - 1) % rowCount);
      nav.requestRender();
      break;
    case InputManager::BTN_DOWN:
      sControlFocus = static_cast<uint8_t>((sControlFocus + 1) % rowCount);
      nav.requestRender();
      break;
    case InputManager::BTN_LEFT:
      if (adjustControl(-1, view, powerOff)) nav.requestRender();
      break;
    case InputManager::BTN_RIGHT:
      if (adjustControl(+1, view, powerOff)) nav.requestRender();
      break;
    case InputManager::BTN_CONFIRM:
      if ((haveState || row == 20 || row == 21) && !controlRowDisabled(row, powerOff)) {
        if (row == 12) {
          // Action row: Confirm triggers the self-clean cycle; inert while a
          // cycle runs (the re-query flips the row back to "Start").
          if (!view.props.selfCleanRunning) applyControlValue(row, 1);
          break;
        }
        if (row == 19) {
          // Action row: the display command only toggles (no explicit set),
          // so every Confirm is a flip request; the row follows displayOn
          // when the service's post-toggle GetState lands.
          AcService::Command command;
          command.kind = AcService::Command::Kind::kToggleDisplay;
          if (!acService.queueCommand(command)) {
            Serial.println("[ui] command queue full, toggle dropped");
            showToast("Command queue full");
          }
          break;
        }
        ControlOptions opts;
        buildControlOptions(row, view, opts);
        sDropdownSel = controlValueIndex(row, opts); // open on the current value
        sDropdownOpen = true;
        nav.requestRender();
      }
      break;
    case InputManager::BTN_BACK:
      nav.pop();
      break;
    default:
      break;
  }
}

// Opaque option-list panel over the rows, current value pre-selected. Temp
// options render through drawTemp for the degree mark. opts carries the
// capability-filtered option list.
void drawOptionDropdown(ui::DisplayTarget& target, const ui::DeviceContext& device, uint8_t row,
                        const ControlOptions& opts) {
  const DropdownLayout layout = dropdownLayout(target, device, opts.count);
  const ui::Rect panel = layout.panel;
  target.fill(panel, ui::Paint::solid(ui::Color::White), 8);
  target.stroke(panel, ui::Paint::solid(ui::Color::Black), 3, 8);

  ui::TextStyle titleStyle{};
  titleStyle.font = ui::FONT_SLOT_SMALL;
  titleStyle.align = ui::TextAlign::Center;
  const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);
  target.text(ui::Rect{panel.x, static_cast<int16_t>(panel.y + 12), panel.width, smallLh}, kControlLabels[row],
              titleStyle);

  for (uint8_t i = 0; i < opts.count; ++i) {
    drawControlItem(target, dropdownItemRect(layout, i), row, opts.values[i], i == sDropdownSel, false);
  }
}

// Leading glyph for each Control row (9.1): generated 1-bpp Lucide icons
// (icons_gen.h), flash-resident const blobs rendered through the DisplayTarget
// bitmap path. The Mode row follows the active mode; every other row is fixed.
const freeink::Icon* controlRowIcon(uint8_t rowId, const midea::AcState& ac) {
  if (rowId == 0) {
    switch (ac.operationalMode) {
      case kModeCool:
        return &icon_mode_cool_24;
      case kModeDry:
        return &icon_mode_dry_24;
      case kModeHeat:
        return &icon_mode_heat_24;
      case kModeFan:
        return &icon_mode_fan_24;
      default:
        return &icon_mode_auto_24;
    }
  }
  constexpr const freeink::Icon* kRowIcons[] = {
      /* 0 (mode): dynamic, above */ nullptr,
      &icon_target_24,
      &icon_fan_speed_24,
      &icon_swing_24,
      &icon_eco_24,
      &icon_turbo_24,
      &icon_ieco_24,
      &icon_out_silent_24,
      &icon_ionizer_24,
      &icon_freeze_prot_24,
      &icon_sleep_24,
      &icon_follow_me_24,
      &icon_self_clean_24,
      &icon_breeze_away_24,
      &icon_breezeless_24,
      &icon_jet_cool_24,
      &icon_power_limit_24,
      &icon_wind_around_24,
      &icon_fresh_air_24,
      &icon_display_24,
      &icon_sound_24,
      &icon_button_lock_24,
  };
  return kRowIcons[rowId];
}

void renderControl(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  drawHeader(target, device, "Control");

  struct ControlRowSpec {
    const char* label;
    const char* value; // null for the temp row: drawn as NN°C
    bool isTemp;
    bool disabled;
  };
  // Rows display the acknowledged snapshot (never the pending desired state);
  // edits land visually when the service's post-send GetState arrives.
  const midea::AcState ac = acService.snapshot();
  const bool haveState = acService.hasState();
  const bool off = haveState && !ac.powerOn;

  // Stale marker in the header: the rows show values from a dead session
  // (the Dashboard carries the full age text). Edits still queue and send
  // once the service reconnects.
  if (haveState && !acService.sessionUp()) {
    ui::TextStyle staleStyle{};
    staleStyle.font = ui::FONT_SLOT_SMALL;
    const int16_t staleLh = target.lineHeight(ui::FONT_SLOT_SMALL);
    const int16_t staleW = target.measureText(ui::FONT_SLOT_SMALL, "stale", staleStyle).width;
    target.text(ui::Rect{static_cast<int16_t>(device.width - kMargin - staleW),
                         static_cast<int16_t>(kMargin + (target.lineHeight(ui::FONT_SLOT_BODY) - staleLh) / 2), staleW,
                         staleLh},
                "stale", staleStyle);
  }

  // Capability gating: iterate the visible rows; unsupported options are
  // filtered and a fully unsupported row (both swing directions false) is
  // hidden entirely.
  const CapView view = controlCaps();
  const uint8_t rowCount = controlVisibleRowCount(view);
  if (sControlFocus >= rowCount) sControlFocus = 0;

  constexpr int16_t kRowHeight = 56;
  constexpr int16_t kRowGap = 12;
  const int16_t top = static_cast<int16_t>(kMargin + target.lineHeight(ui::FONT_SLOT_BODY) + 20);
  const int16_t availH = static_cast<int16_t>(buttonHintsTop(target, device) - 8 - top);
  // Shrink-to-fit (8.3): with every row visible (property rows included) the
  // fixed-height block overflows the 480 px landscape view into the hint
  // chips; compress the rows (never below one body line + padding) so the
  // block ends above them.
  const int16_t fitH = static_cast<int16_t>((availH + kRowGap) / static_cast<int16_t>(rowCount) - kRowGap);
  int16_t rowHeight = fitH < kRowHeight ? fitH : kRowHeight;
  const int16_t minRowHeight = static_cast<int16_t>(target.lineHeight(ui::FONT_SLOT_BODY) + 8);
  if (rowHeight < minRowHeight) rowHeight = minRowHeight;
  // Scroll window (8.4): at the floor height the landscape band holds ~7 of
  // the 12 possible rows; only the band around the focused row is drawn and
  // the scroll keeps the focus inside it (no-op whenever everything fits).
  int16_t visibleSpan = static_cast<int16_t>((availH + kRowGap) / (rowHeight + kRowGap));
  if (visibleSpan < 1) visibleSpan = 1;
  if (visibleSpan > static_cast<int16_t>(rowCount)) visibleSpan = static_cast<int16_t>(rowCount);
  if (sControlFocus < sControlScroll) sControlScroll = sControlFocus;
  if (sControlFocus >= sControlScroll + visibleSpan) {
    sControlScroll = static_cast<uint8_t>(sControlFocus - visibleSpan + 1);
  }
  for (int16_t slot = 0; slot < visibleSpan; ++slot) {
    const uint8_t index = static_cast<uint8_t>(sControlScroll + slot);
    const uint8_t rowId = controlVisibleRowAt(index, view);
    ControlRowSpec spec{};
    char textBuf[20];
    if (rowId == 20) {
      // Setting-backed (8.8): the value exists before any session.
      spec = {kControlLabels[20], settingsStore.settings().ac.beep ? "On" : "Off", false, false};
    } else if (rowId == 21) {
      // Setting-backed (8.9): the override lives in the service; the row
      // shows the flag itself, before and without a session.
      spec = {kControlLabels[21], settingsStore.settings().ac.buttonLock ? "On" : "Off", false, false};
    } else if (!haveState) {
      spec = {kControlLabels[rowId], "no data", false, false};
    } else {
      switch (rowId) {
        case 0:
          spec = {kControlLabels[0], off ? "Off" : modeName(ac.operationalMode), false, false};
          break;
        case 1:
          spec = {kControlLabels[1], nullptr, true, off};
          break;
        case 2:
          spec = {kControlLabels[2], fanName(ac.fanSpeed, textBuf, sizeof(textBuf)), false, off};
          break;
        case 3:
          spec = {kControlLabels[3], swingName(ac.swingMode), false, off};
          break;
        case 4:
          spec = {kControlLabels[4], ac.eco ? "On" : "Off", false, false};
          break;
        case 5:
          spec = {kControlLabels[5], ac.turbo ? "On" : "Off", false, false};
          break;
        case 6:
          spec = {kControlLabels[6], view.props.ieco ? "On" : "Off", false, false};
          break;
        case 7:
          spec = {kControlLabels[7], view.props.outSilent ? "On" : "Off", false, false};
          break;
        case 8:
          spec = {kControlLabels[8], ac.purifier ? "On" : "Off", false, false};
          break;
        case 9:
          spec = {kControlLabels[9], ac.freezeProtection ? "On" : "Off", false, false};
          break;
        case 10:
          spec = {kControlLabels[10], ac.sleep ? "On" : "Off", false, false};
          break;
        case 11:
          spec = {kControlLabels[11], ac.followMe ? "On" : "Off", false, false};
          break;
        case 12:
          spec = {kControlLabels[12], view.props.selfCleanRunning ? "Running..." : "Start", false, false};
          break;
        case 13:
          spec = {kControlLabels[13], view.props.breezeAway ? "On" : "Off", false, false};
          break;
        case 14:
          spec = {kControlLabels[14], view.props.breezeless ? "On" : "Off", false, false};
          break;
        case 15:
          spec = {kControlLabels[15], view.props.flash ? "On" : "Off", false, false};
          break;
        case 16:
          spec = {kControlLabels[16], rateSelectName(view.props.rateSelect, textBuf, sizeof(textBuf)), false, false};
          break;
        case 17:
          spec = {kControlLabels[17],
                  view.props.cascade == 1   ? "Up"
                  : view.props.cascade == 2 ? "Down"
                                            : "Off",
                  false, false};
          break;
        case 18:
          spec = {kControlLabels[18], freshAirName(view.props.freshAir, textBuf, sizeof(textBuf)), false, false};
          break;
        case 19:
          spec = {kControlLabels[19], ac.displayOn ? "On" : "Off", false, false};
          break;
      }
    }
    const bool focused = index == sControlFocus;
    const ui::Rect rowRect{kMargin, static_cast<int16_t>(top + slot * (rowHeight + kRowGap)),
                           static_cast<int16_t>(device.width - 2 * kMargin), rowHeight};
    if (focused) {
      target.fill(rowRect, ui::Paint::solid(ui::Color::Black), 6);
    } else {
      target.stroke(rowRect, ui::Paint::solid(ui::Color::Black), 2, 6);
    }

    // Leading icon (9.1): fixed 24 px column left of the label, optically
    // centered on the row; inverted ink keeps it visible on the focused row.
    const freeink::Icon* icon = controlRowIcon(rowId, ac);
    target.bitmap(ui::Rect{static_cast<int16_t>(rowRect.x + kRowPad),
                           static_cast<int16_t>(rowRect.y + rowRect.height / 2 - icon->opticalCenterY), kIconSize,
                           kIconSize},
                  ui::bitmapFromIcon(*icon), ui::BitmapMode::Center,
                  ui::Paint::solid(focused ? ui::Color::White : ui::Color::Black));

    ui::TextStyle labelStyle{};
    labelStyle.font = ui::FONT_SLOT_SMALL;
    labelStyle.inverted = focused;
    const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);
    target.text(ui::Rect{static_cast<int16_t>(rowRect.x + kRowPad + kIconCol),
                         static_cast<int16_t>(rowRect.y + (rowRect.height - smallLh) / 2),
                         static_cast<int16_t>(rowRect.width - kIconCol), smallLh},
                spec.label, labelStyle);

    if (spec.isTemp && !spec.disabled) {
      const uint8_t halfSteps = tempHalfSteps(ac.targetTemperature);
      drawTemp(target,
               ui::Rect{static_cast<int16_t>(rowRect.x + kRowPad + kIconCol), rowRect.y,
                        static_cast<int16_t>(rowRect.width - 2 * kRowPad - kIconCol), rowRect.height},
               halfSteps / 2, false, focused, (halfSteps & 1) != 0);
      continue;
    }

    // Disabled rows render parenthesized (or "--" for temp) as the inert cue.
    char valueBuf[24];
    const char* base = spec.isTemp ? "--" : spec.value;
    if (spec.disabled) {
      snprintf(valueBuf, sizeof(valueBuf), "(%s)", base);
    } else {
      snprintf(valueBuf, sizeof(valueBuf), "%s", base);
    }

    ui::TextStyle valueStyle{};
    valueStyle.font = ui::FONT_SLOT_BODY;
    valueStyle.inverted = focused;
    const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
    const int16_t valueW = target.measureText(ui::FONT_SLOT_BODY, valueBuf, valueStyle).width;
    const int16_t valueX = static_cast<int16_t>(rowRect.x + rowRect.width - kRowPad - valueW);
    target.text(ui::Rect{valueX, static_cast<int16_t>(rowRect.y + (rowRect.height - lh) / 2), valueW, lh}, valueBuf,
                valueStyle);
  }

  // Scroll position cue, compact font, only while rows hide below the band.
  if (visibleSpan < static_cast<int16_t>(rowCount)) {
    const ui::FontId compactFont = freedea::kFontSlotCompact;
    const int16_t compactLh = target.lineHeight(compactFont);
    char posBuf[12];
    snprintf(posBuf, sizeof(posBuf), "%u/%u", static_cast<unsigned>(sControlFocus + 1),
             static_cast<unsigned>(rowCount));
    ui::TextStyle posStyle{};
    posStyle.font = compactFont;
    posStyle.align = ui::TextAlign::Right;
    target.text(ui::Rect{kMargin, static_cast<int16_t>(buttonHintsTop(target, device) - compactLh - 2),
                         static_cast<int16_t>(device.width - 2 * kMargin), compactLh},
                posBuf, posStyle);
  }

  if (sDropdownOpen) {
    ControlOptions opts;
    buildControlOptions(controlVisibleRowAt(sControlFocus, view), view, opts);
    drawOptionDropdown(target, device, controlVisibleRowAt(sControlFocus, view), opts);
    setButtonHints({.back = "Cancel", .confirm = "Save"});
  } else {
    setButtonHints({.back = "Done", .confirm = "Open", .left = "-", .right = "+"});
  }
}

// --- Devices (discovery, 4.4) ---------------------------------------------

// Screen-local flow: saved list (device rows + a trailing "Scan for devices"
// action row) -> LAN scan -> results -> (Confirm) add confirmation. Confirm
// on a device row opens a delete confirmation; a committed removal reboots
// (persist-then-reboot, same contract as the portal) because AcService only
// re-picks its target at boot. Discovery never yields token/key: V2 devices
// add directly, V3 devices are saved without credentials and the flow tells
// the user to enter them in the web portal.
enum class DevMode : uint8_t { kSaved, kScanning, kResults, kConfirm, kConfirmDelete };
DevMode sDevMode = DevMode::kSaved;
uint8_t sDevCursor = 0;  // cursor over the discovered list
uint8_t sDevVisible = 1; // rows that fit on screen; refreshed every render
char sDevNote[48] = {};  // one-shot status line shown on the saved list

constexpr int16_t kDevRowHeight = 56;
constexpr int16_t kDevRowGap = 10;

// Dotted-quad parse without sscanf (scanf drags a second printf-family
// front-end into the image).
bool parseIpBytes(const char* text, uint8_t out[4]) {
  int field = 0;
  unsigned octet = 0;
  const char* p = text;
  for (;; ++p) {
    if (*p >= '0' && *p <= '9') {
      octet = octet * 10 + static_cast<unsigned>(*p - '0');
      if (octet > 255) return false;
    } else if (*p == '.') {
      if (field == 3 || p == text || p[-1] == '.') return false;
      out[field++] = static_cast<uint8_t>(octet);
      octet = 0;
    } else if (*p == '\0') {
      break;
    } else {
      return false;
    }
  }
  if (field != 3) return false;
  out[3] = static_cast<uint8_t>(octet);
  return true;
}

bool storedHasId(uint64_t id) {
  const devices::List& list = deviceStore.list();
  for (uint8_t i = 0; i < list.count; ++i) {
    if (list.devices[i].id == id) return true;
  }
  return false;
}

// Row text staging for the two lists (labels + right-aligned hints). Static
// like the Hotspot QR scratch: the loop-task stack is tight, and rendering
// only ever runs on the main loop. Sized for the largest list (discovery).
constexpr uint8_t kDevMaxRows = DeviceDiscovery::kMaxResults;
constexpr size_t kDevHintWidth = 28; // worst case "V3 255.255.255.255 saved" + NUL
const char* sDevLabels[kDevMaxRows];
char sDevHints[kDevMaxRows][kDevHintWidth];

// Selectable rows shared by the saved and discovered lists from the staged
// sDevLabels/sDevHints: fills the band between rowsTop and the footer,
// clamps the cursor to what fits, and summarizes rows past the last visible
// one as "+N more".
void drawDeviceRows(ui::DisplayTarget& target, const ui::DeviceContext& device, int16_t rowsTop, uint8_t count,
                    uint8_t& cursor) {
  const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);
  const int16_t rowsBottom = device.height - kMargin - smallLh - 8; // footer clearance, as on the dashboard
  int32_t visible = (rowsBottom - rowsTop + kDevRowGap) / (kDevRowHeight + kDevRowGap);
  if (visible < 1) visible = 1;
  sDevVisible = static_cast<uint8_t>(visible);
  const uint8_t shown = count < sDevVisible ? count : sDevVisible;
  if (cursor >= shown) cursor = 0;

  for (uint8_t i = 0; i < shown; ++i) {
    const ui::Rect row{kMargin, static_cast<int16_t>(rowsTop + i * (kDevRowHeight + kDevRowGap)),
                       static_cast<int16_t>(device.width - 2 * kMargin), kDevRowHeight};
    drawMenuRow(target, row, sDevLabels[i], sDevHints[i], i == cursor);
  }
  if (count > shown) {
    ui::TextStyle style{};
    style.font = ui::FONT_SLOT_SMALL;
    style.align = ui::TextAlign::Center;
    char more[16];
    snprintf(more, sizeof(more), "+%u more", static_cast<unsigned>(count - shown));
    target.text(
        ui::Rect{0, static_cast<int16_t>(rowsTop + shown * (kDevRowHeight + kDevRowGap)), device.width, smallLh}, more,
        style);
  }
}

// Saved-device row hint: "V3 192.0.2.100" / "V3 no IP".
void savedDeviceHint(const devices::Device& d, char* buf, size_t cap) {
  if ((d.ip[0] | d.ip[1] | d.ip[2] | d.ip[3]) == 0) {
    snprintf(buf, cap, "V%u no IP", static_cast<unsigned>(d.version));
  } else {
    snprintf(buf, cap, "V%u %u.%u.%u.%u", static_cast<unsigned>(d.version), static_cast<unsigned>(d.ip[0]),
             static_cast<unsigned>(d.ip[1]), static_cast<unsigned>(d.ip[2]), static_cast<unsigned>(d.ip[3]));
  }
}

// Trailing action row of the saved list (cursor-selectable like a device).
constexpr const char kDevScanAction[] = "Scan for devices";

void renderDeviceSaved(ui::DisplayTarget& target, const ui::DeviceContext& device) {
  const devices::List& list = deviceStore.list();
  const uint8_t n = list.count < kDevMaxRows - 1 ? list.count : kDevMaxRows - 1;
  for (uint8_t i = 0; i < n; ++i) {
    sDevLabels[i] = list.devices[i].name;
    savedDeviceHint(list.devices[i], sDevHints[i], kDevHintWidth);
  }
  sDevLabels[n] = kDevScanAction;
  sDevHints[n][0] = '\0';
  drawDeviceRows(target, device, static_cast<int16_t>(device.height / 4), static_cast<uint8_t>(n + 1), sDevCursor);

  // One-shot status line (add result) just above the footer.
  if (sDevNote[0] != '\0') {
    ui::TextStyle style{};
    style.font = ui::FONT_SLOT_SMALL;
    style.align = ui::TextAlign::Center;
    const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);
    target.text(ui::Rect{0, static_cast<int16_t>(device.height - 2 * smallLh - kMargin - 6), device.width, smallLh},
                sDevNote, style);
  }
  setButtonHints({.back = "Back", .confirm = "Open", .up = "Up", .down = "Down"});
}

void renderDevices(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  // The scan runs off the main loop; completion lands here (the loop only
  // requests the repaint, it does not own the screen mode).
  if (sDevMode == DevMode::kScanning && !deviceDiscovery.scanning()) sDevMode = DevMode::kResults;

  drawHeader(target, device, "Devices");
  const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);

  switch (sDevMode) {
    case DevMode::kSaved:
      renderDeviceSaved(target, device);
      return;

    case DevMode::kScanning: {
      const int16_t center = device.height / 2;
      drawBodyLine(target, device, center, "Scanning the LAN...");
      setButtonHints({.back = "Cancel"});
      return;
    }

    case DevMode::kResults: {
      const size_t count = deviceDiscovery.count();
      ui::TextStyle small{};
      small.font = ui::FONT_SLOT_SMALL;
      small.align = ui::TextAlign::Center;
      char line[32];
      snprintf(line, sizeof(line), "Found %u device(s)", static_cast<unsigned>(count));
      const int16_t rowsTop = device.height / 4;
      target.text(ui::Rect{0, rowsTop, device.width, smallLh}, line, small);
      if (count == 0) {
        drawBodyLine(target, device, static_cast<int16_t>(device.height / 2), "No devices found");
        setButtonHints({.back = "Close"});
        return;
      }
      const uint8_t n = count < kDevMaxRows ? static_cast<uint8_t>(count) : kDevMaxRows;
      for (uint8_t i = 0; i < n; ++i) {
        const midea::DiscoveryDeviceInfo& info = deviceDiscovery.at(i);
        sDevLabels[i] = info.name;
        snprintf(sDevHints[i], kDevHintWidth, "V%u %s%s", static_cast<unsigned>(info.version), info.ip,
                 storedHasId(info.deviceId) ? " saved" : "");
      }
      drawDeviceRows(target, device, static_cast<int16_t>(rowsTop + smallLh + 10), n, sDevCursor);
      setButtonHints({.back = "Close", .confirm = "Add", .up = "Up", .down = "Down"});
      return;
    }

    case DevMode::kConfirm: {
      if (deviceDiscovery.count() == 0) { // defensive: results vanished (cannot normally happen)
        sDevMode = DevMode::kSaved;
        setButtonHints({.back = "Back", .confirm = "Open", .up = "Up", .down = "Down"});
        return;
      }
      const midea::DiscoveryDeviceInfo& info = deviceDiscovery.at(sDevCursor);
      const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
      const int16_t center = device.height / 2;
      char line[48];
      drawBodyLine(target, device, static_cast<int16_t>(center - 3 * lh), info.name);
      snprintf(line, sizeof(line), "ID %" PRIu64, info.deviceId);
      drawBodyLine(target, device, static_cast<int16_t>(center - 2 * lh), line);
      snprintf(line, sizeof(line), "%s port %u", info.ip, static_cast<unsigned>(info.port));
      drawBodyLine(target, device, static_cast<int16_t>(center - lh), line);
      if (info.version == midea::DiscoveryVersion::kV3) {
        drawBodyLine(target, device, center, "V3: token + key required,");
        drawBodyLine(target, device, static_cast<int16_t>(center + lh), "enter them in the web portal");
        drawBodyLine(target, device, static_cast<int16_t>(center + 2 * lh), "after adding.");
      } else {
        drawBodyLine(target, device, center, "V2: no credentials needed");
      }
      setButtonHints({.back = "Cancel", .confirm = "Add"});
      return;
    }

    case DevMode::kConfirmDelete: {
      const devices::List& list = deviceStore.list();
      if (sDevCursor >= list.count) { // defensive: store shrank underneath
        sDevMode = DevMode::kSaved;
        setButtonHints({.back = "Back", .confirm = "Open", .up = "Up", .down = "Down"});
        return;
      }
      const devices::Device& d = list.devices[sDevCursor];
      const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
      const int16_t center = device.height / 2;
      char line[48];
      drawBodyLine(target, device, static_cast<int16_t>(center - 2 * lh), d.name);
      snprintf(line, sizeof(line), "ID %" PRIu64, d.id);
      drawBodyLine(target, device, static_cast<int16_t>(center - lh), line);
      drawBodyLine(target, device, center, "Remove this device?");
      drawBodyLine(target, device, static_cast<int16_t>(center + lh), "The unit reboots after removal.");
      setButtonHints({.back = "Cancel", .confirm = "Remove"});
      return;
    }
  }
}

// Add the discovered device to the store (no token/key) and persist.
// AcService picks its target at boot, so the note spells out the follow-up.
void addDiscoveredDevice(Navigation& nav) {
  const midea::DiscoveryDeviceInfo& info = deviceDiscovery.at(sDevCursor);
  if (storedHasId(info.deviceId)) {
    snprintf(sDevNote, sizeof(sDevNote), "Already in your devices");
    sDevMode = DevMode::kSaved;
    nav.requestRender();
    return;
  }
  if (deviceStore.list().count >= devices::kMaxDevices) {
    snprintf(sDevNote, sizeof(sDevNote), "Device list full (%u)", static_cast<unsigned>(devices::kMaxDevices));
    sDevMode = DevMode::kSaved;
    nav.requestRender();
    return;
  }

  devices::Device d;
  d.id = info.deviceId;
  d.version = static_cast<uint8_t>(info.version);
  d.port = info.port != 0 ? info.port : devices::kDefaultPort;
  if (!parseIpBytes(info.ip, d.ip)) memset(d.ip, 0, sizeof(d.ip));
  size_t nameLen = strlen(info.name);
  if (nameLen >= devices::kNameCapacity) nameLen = devices::kNameCapacity - 1;
  memcpy(d.name, info.name, nameLen);
  d.name[nameLen] = '\0';

  const int index = deviceStore.upsert(d);
  const bool saved = index >= 0 && deviceStore.saveNow();
  Serial.printf("[DEV] discovery %s id=%" PRIu64 " v%u ip=%s\n", saved ? "added" : "add failed", info.deviceId,
                static_cast<unsigned>(info.version), info.ip);
  if (!saved) {
    // Notes only render on the saved list, so land there to surface this;
    // a retry is just another scan (the results table is still populated).
    snprintf(sDevNote, sizeof(sDevNote), "Add failed, see serial log");
    sDevMode = DevMode::kSaved;
  } else if (info.version == midea::DiscoveryVersion::kV3) {
    snprintf(sDevNote, sizeof(sDevNote), "Saved. Enter token + key in portal");
    sDevMode = DevMode::kSaved;
  } else {
    snprintf(sDevNote, sizeof(sDevNote), "Saved. Reboot to connect");
    sDevMode = DevMode::kSaved;
  }
  nav.requestRender();
}

void devicesButton(Navigation& nav, uint8_t button) {
  aliasSideToNav(button); // list modes navigate with the right bar too
  if (sDevMode == DevMode::kSaved) {
    // Cursor spans the device rows plus the trailing scan action row.
    const uint8_t rows = static_cast<uint8_t>(deviceStore.list().count + 1);
    const uint8_t shown = rows < sDevVisible ? rows : sDevVisible;
    if (button == InputManager::BTN_UP) {
      sDevCursor = static_cast<uint8_t>((sDevCursor + shown - 1) % shown);
      nav.requestRender();
    } else if (button == InputManager::BTN_DOWN) {
      sDevCursor = static_cast<uint8_t>((sDevCursor + 1) % shown);
      nav.requestRender();
    } else if (button == InputManager::BTN_CONFIRM) {
      if (sDevCursor == deviceStore.list().count) { // the scan action row
        if (wifi.link() != WifiService::Link::kConnected) {
          snprintf(sDevNote, sizeof(sDevNote), "WiFi not connected");
        } else if (deviceDiscovery.start()) {
          sDevMode = DevMode::kScanning;
        } else {
          snprintf(sDevNote, sizeof(sDevNote), "Scan failed, see serial log");
        }
      } else {
        sDevMode = DevMode::kConfirmDelete;
      }
      nav.requestRender();
    } else if (button == InputManager::BTN_BACK) {
      nav.pop();
    }
    return;
  }

  if (sDevMode == DevMode::kConfirmDelete) {
    if (button == InputManager::BTN_BACK) {
      sDevMode = DevMode::kSaved;
      nav.requestRender();
    } else if (button == InputManager::BTN_CONFIRM) {
      const devices::Device& d = deviceStore.list().devices[sDevCursor];
      Serial.printf("[DEV] removing id=%" PRIu64 " name=%s\n", static_cast<unsigned long long>(d.id), d.name);
      const uint64_t id = d.id; // the reference goes stale once the list compacts
      if (deviceStore.remove(id) && deviceStore.saveNow()) {
        // Persist-then-reboot (portal contract): the reboot atomically
        // applies the removal and lets AcService re-pick its target.
        ESP.restart();
      }
      snprintf(sDevNote, sizeof(sDevNote), "Remove failed, see serial log");
      sDevMode = DevMode::kSaved;
      nav.requestRender();
    }
    return;
  }

  if (sDevMode == DevMode::kScanning) {
    if (button == InputManager::BTN_BACK) {
      deviceDiscovery.cancel();
      sDevMode = DevMode::kSaved;
      nav.requestRender();
    }
    return;
  }

  if (sDevMode == DevMode::kResults) {
    // Cursor wraps over what is both present and on screen (sDevVisible is
    // refreshed by every drawDeviceRows pass).
    const uint8_t count = static_cast<uint8_t>(deviceDiscovery.count());
    const uint8_t shown = count < sDevVisible ? count : sDevVisible;
    switch (button) {
      case InputManager::BTN_UP:
        if (shown > 0) sDevCursor = static_cast<uint8_t>((sDevCursor + shown - 1) % shown);
        nav.requestRender();
        break;
      case InputManager::BTN_DOWN:
        if (shown > 0) sDevCursor = static_cast<uint8_t>((sDevCursor + 1) % shown);
        nav.requestRender();
        break;
      case InputManager::BTN_CONFIRM:
        if (count > 0) {
          if (storedHasId(deviceDiscovery.at(sDevCursor).deviceId)) {
            snprintf(sDevNote, sizeof(sDevNote), "Already in your devices");
            sDevMode = DevMode::kSaved;
          } else {
            sDevMode = DevMode::kConfirm;
          }
          nav.requestRender();
        }
        break;
      case InputManager::BTN_BACK:
        sDevMode = DevMode::kSaved;
        nav.requestRender();
        break;
      default:
        break;
    }
    return;
  }

  // kConfirm
  if (button == InputManager::BTN_CONFIRM) {
    addDiscoveredDevice(nav);
  } else if (button == InputManager::BTN_BACK) {
    sDevMode = DevMode::kResults;
    nav.requestRender();
  }
}

// --- Settings / About ---------------------------------------------------------

constexpr const char* kSettingsItems[] = {"WiFi", "Devices", "Weather", "Hotspot", "Web portal", "WireGuard"};
// Index 0 (WiFi) shows the live link status instead of a static hint.
constexpr const char* kSettingsHints[] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
constexpr uint8_t kSettingsItemCount = sizeof(kSettingsItems) / sizeof(kSettingsItems[0]);
constexpr uint8_t kWifiRow = 0;
constexpr uint8_t kDevicesRow = 1;
constexpr uint8_t kWeatherRow = 2;
constexpr uint8_t kHotspotRow = 3;
constexpr uint8_t kWebPortalRow = 4;
constexpr uint8_t kWireGuardRow = 5;
static_assert(sizeof(kSettingsHints) / sizeof(kSettingsHints[0]) == kSettingsItemCount,
              "kSettingsHints must match kSettingsItems");

// Cursor persists across visits, like Home's.
uint8_t sSettingsSelection = 0;

// Right-aligned status hint for the WiFi row. ASCII only: the UI font covers
// U+0020-007E, so "Connecting..." uses three periods, not an ellipsis glyph.
// Weather row hint (6.2): config state or the age of the last successful
// fetch. Ages only with repaints (the update edge repaints Settings too).
const char* weatherStatusHint(char* buf, size_t cap) {
  const settings::Weather& w = settingsStore.settings().weather;
  if (!w.enabled) return "off";
  if (!settings::weatherConfigured(w)) return "no location";
  const uint32_t age = weatherService.lastFetchAgeMs();
  if (age == 0xFFFFFFFFu) return "no data yet";
  const uint32_t minutes = age / 60000u;
  if (minutes < 60) {
    snprintf(buf, cap, "%u min", static_cast<unsigned>(minutes));
  } else {
    snprintf(buf, cap, "%u h %02u m", static_cast<unsigned>(minutes / 60), static_cast<unsigned>(minutes % 60));
  }
  return buf;
}

// Web portal row hint (6.3): the full URL while listening with a lease, a
// short status otherwise. "off" is the boot state — the portal is runtime-
// only and never persists across a reboot.
const char* webPortalStatusHint(char* buf, size_t cap) {
  if (!portal.enabled()) return "off";
  if (!wifi.connected()) return "no ip";
  char ip[16];
  wifi.localIp(ip, sizeof(ip));
  snprintf(buf, cap, "http://%s", ip);
  return buf;
}

// WireGuard row hint (7.3): config state first, then tunnel state. Confirm
// on this row toggles wg.enabled.
const char* wireGuardStatusHint(char* buf, size_t cap) {
  const settings::WireGuard& wg = settingsStore.settings().wireguard;
  if (!wg.enabled) return "off";
  if (!settings::wireGuardConfigured(wg)) return "incomplete";
  switch (wgService.status()) {
    case WireGuardService::Status::kConnecting:
      return "connecting...";
    case WireGuardService::Status::kHandshaking:
      return "handshaking";
    case WireGuardService::Status::kUp:
      snprintf(buf, cap, "up %s", wg.ownIp);
      return buf;
    case WireGuardService::Status::kFailed:
      return "failed";
    case WireGuardService::Status::kDisabled:
      break;
  }
  return wifi.connected() ? "waiting" : "wifi down";
}

// Portal-cache suffix (7.2c) for the cached connectivity state of a profile;
// nullptr when it was never probed (or the probe failed), so nothing shows.
const char* portalStateHint(uint8_t state) {
  if (state == settings::kPortalOpen) return "internet ok";
  if (state == settings::kPortalFound) return "login needed";
  return nullptr;
}

const char* wifiStatusHint(char* buf, size_t cap) {
  switch (wifi.link()) {
    case WifiService::Link::kConnected: {
      char ip[16];
      wifi.localIp(ip, sizeof(ip));
      // The active profile's cached portal state trails the lease, e.g.
      // "Connected 10.0.0.5 - login needed".
      const settings::Network* active = settings::activeNetwork(settingsStore.settings());
      const char* portalHint = active != nullptr ? portalStateHint(active->portalState) : nullptr;
      if (portalHint != nullptr) {
        snprintf(buf, cap, "Connected %s - %s", ip, portalHint);
      } else {
        snprintf(buf, cap, "Connected %s", ip);
      }
      return buf;
    }
    case WifiService::Link::kConnecting:
      return "Connecting...";
    case WifiService::Link::kDisconnected:
      return "Disconnected";
    case WifiService::Link::kDisabled:
      break;
  }
  return "Not configured";
}

void renderSettings(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  drawHeader(target, device, "Settings");

  constexpr int16_t kRowGap = 12;
  // Rows fill the band between header and footer, capped at 64 px: six
  // items still fit the 480 px-tall landscape orientation (rows shrink to
  // ~50 px) without running under the footer.
  const int16_t rowsTop = static_cast<int16_t>(kMargin + target.lineHeight(ui::FONT_SLOT_TITLE) + 12);
  const int16_t rowsBottom =
      static_cast<int16_t>(device.height - kMargin - target.lineHeight(ui::FONT_SLOT_SMALL) - 12);
  const int16_t fit = (rowsBottom - rowsTop - (kSettingsItemCount - 1) * kRowGap) / kSettingsItemCount;
  const int16_t rowHeight = fit < 64 ? fit : 64;
  if (sSettingsSelection >= kSettingsItemCount) sSettingsSelection = 0;
  // Fits "Connected 255.255.255.255 - login needed" (7.2c suffix included).
  char wifiHint[48];
  char devHint[16];
  char wxHint[20];
  char webHint[32];
  char wgHint[24];
  snprintf(devHint, sizeof(devHint), "%u saved", static_cast<unsigned>(deviceStore.list().count));
  for (uint8_t i = 0; i < kSettingsItemCount; ++i) {
    const ui::Rect row{kMargin, static_cast<int16_t>(rowsTop + i * (rowHeight + kRowGap)),
                       static_cast<int16_t>(device.width - 2 * kMargin), rowHeight};
    // WiFi carries the live link status, Devices the saved count, Weather
    // the fetch age, Web portal the live URL, WireGuard the tunnel state.
    const char* hint = i == kWifiRow        ? wifiStatusHint(wifiHint, sizeof(wifiHint))
                       : i == kDevicesRow   ? devHint
                       : i == kWeatherRow   ? weatherStatusHint(wxHint, sizeof(wxHint))
                       : i == kWebPortalRow ? webPortalStatusHint(webHint, sizeof(webHint))
                       : i == kWireGuardRow ? wireGuardStatusHint(wgHint, sizeof(wgHint))
                                            : kSettingsHints[i];
    drawMenuRow(target, row, kSettingsItems[i], hint, i == sSettingsSelection);
  }

  setButtonHints({.back = "Back", .confirm = "Open", .up = "Up", .down = "Down"});
}

void settingsButton(Navigation& nav, uint8_t button) {
  aliasSideToNav(button);
  switch (button) {
    case InputManager::BTN_UP:
      sSettingsSelection = static_cast<uint8_t>((sSettingsSelection + kSettingsItemCount - 1) % kSettingsItemCount);
      nav.requestRender();
      break;
    case InputManager::BTN_DOWN:
      sSettingsSelection = static_cast<uint8_t>((sSettingsSelection + 1) % kSettingsItemCount);
      nav.requestRender();
      break;
    case InputManager::BTN_CONFIRM:
      // WiFi opens the saved-network selector (7.2b). Devices opens the
      // discovery screen with a fresh state (any finished scan is dropped).
      // Web portal toggles the on-demand LAN portal (6.3). Hotspot writes the
      // provision flag and reboots into the AP-only provisioning boot (the
      // recovery path when saved credentials lock us out).
      if (sSettingsSelection == kWifiRow) {
        nav.push(ScreenId::Networks);
      } else if (sSettingsSelection == kDevicesRow) {
        deviceDiscovery.cancel();
        sDevMode = DevMode::kSaved;
        sDevCursor = 0;
        sDevNote[0] = '\0';
        nav.push(ScreenId::Devices);
      } else if (sSettingsSelection == kWeatherRow) {
        // Manual fetch (6.2): kicks the task out of its schedule wait; the
        // age hint and the Dashboard panel follow on completion (or the
        // failure surfaces as a toast).
        weatherService.requestRefresh();
        nav.requestRender();
      } else if (sSettingsSelection == kWebPortalRow) {
        // Runtime-only toggle (6.3): never persisted, so a reboot always
        // comes back with the portal closed. An enabled portal closes here;
        // the 15-min idle timeout in WebPortal::tick() is the safety net.
        if (portal.enabled()) {
          portal.end();
        } else {
          portal.begin(settingsStore, deviceStore, true);
        }
        nav.requestRender();
      } else if (sSettingsSelection == kWireGuardRow) {
        // Toggle (7.3): flips wg.enabled in the live settings. The service's
        // config diff tears the tunnel down (off) or brings it up with a
        // reset backoff (on), so no explicit kick is needed. Like the
        // Networks row, a failed save only costs durability across reboots.
        settings::Settings& s = settingsStore.settings();
        if (!s.wireguard.enabled && !settings::wireGuardConfigured(s.wireguard)) {
          showToast("Set up in the web portal first");
        } else {
          s.wireguard.enabled = !s.wireguard.enabled;
          if (!settingsStore.saveNow()) showToast("Save failed - not persisted");
        }
        nav.requestRender();
      } else if (sSettingsSelection == kHotspotRow && settingsStore.requestProvision()) {
        ESP.restart();
      }
      break;
    case InputManager::BTN_BACK:
      nav.pop();
      break;
    default:
      break;
  }
}

// --- Networks (7.2b) ------------------------------------------------------------

// Saved WiFi profiles: pick which one the STA connects to. Profiles are
// added/edited/deleted through the web portal; this is the on-device
// selector. Cursor persists across visits, like the other lists.
uint8_t sNetworksSelection = 0;

// Fills `slots` with the indices of the non-empty profiles; returns the
// count. Empty slots can exist anywhere in the fixed 4-slot store.
uint8_t filledNetworkSlots(uint8_t* slots, uint8_t cap) {
  const settings::Settings& s = settingsStore.settings();
  uint8_t n = 0;
  for (uint8_t i = 0; i < settings::kMaxNetworks && n < cap; ++i) {
    if (s.networks[i].ssid[0] != '\0') slots[n++] = i;
  }
  return n;
}

void renderNetworks(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  drawHeader(target, device, "WiFi networks");

  uint8_t slots[settings::kMaxNetworks];
  const uint8_t count = filledNetworkSlots(slots, settings::kMaxNetworks);
  if (sNetworksSelection >= count) sNetworksSelection = 0;

  // Only reachable with an empty store on a provisioning/recovery boot.
  if (count == 0) {
    drawPassiveNote(target, device, "No networks - add one via the web portal");
    setButtonHints({.back = "Back"});
    return;
  }

  constexpr int16_t kRowGap = 12;
  const int16_t rowsTop = static_cast<int16_t>(kMargin + target.lineHeight(ui::FONT_SLOT_TITLE) + 12);
  const int16_t rowsBottom =
      static_cast<int16_t>(device.height - kMargin - target.lineHeight(ui::FONT_SLOT_SMALL) - 12);
  const int16_t fit = (rowsBottom - rowsTop - (count - 1) * kRowGap) / count;
  const int16_t rowHeight = fit < 64 ? fit : 64;

  const settings::Settings& s = settingsStore.settings();
  for (uint8_t i = 0; i < count; ++i) {
    const ui::Rect row{kMargin, static_cast<int16_t>(rowsTop + i * (rowHeight + kRowGap)),
                       static_cast<int16_t>(device.width - 2 * kMargin), rowHeight};
    // Hint: the active marker, the cached portal state (7.2c), or both.
    const bool isActive = slots[i] == s.activeNetwork;
    const char* portalHint = portalStateHint(s.networks[slots[i]].portalState);
    char hintBuf[40];
    const char* hint;
    if (isActive && portalHint != nullptr) {
      snprintf(hintBuf, sizeof(hintBuf), "active - %s", portalHint);
      hint = hintBuf;
    } else if (isActive) {
      hint = "active";
    } else {
      hint = portalHint;
    }
    drawMenuRow(target, row, s.networks[slots[i]].ssid, hint, i == sNetworksSelection);
  }

  setButtonHints({.back = "Back", .confirm = "Connect", .up = "Up", .down = "Down"});
}

void networksButton(Navigation& nav, uint8_t button) {
  aliasSideToNav(button);
  uint8_t slots[settings::kMaxNetworks];
  const uint8_t count = filledNetworkSlots(slots, settings::kMaxNetworks);
  if (sNetworksSelection >= count) sNetworksSelection = 0;

  switch (button) {
    case InputManager::BTN_UP:
      if (count > 0) sNetworksSelection = static_cast<uint8_t>((sNetworksSelection + count - 1) % count);
      nav.requestRender();
      break;
    case InputManager::BTN_DOWN:
      if (count > 0) sNetworksSelection = static_cast<uint8_t>((sNetworksSelection + 1) % count);
      nav.requestRender();
      break;
    case InputManager::BTN_CONFIRM: {
      if (count == 0) break;
      settings::Settings& s = settingsStore.settings();
      const uint8_t slot = slots[sNetworksSelection];
      if (slot == s.activeNetwork) break; // already the one in use
      s.activeNetwork = slot;
      // The switch applies to the live settings either way; a failed save
      // only means it will not survive the next reboot.
      if (!settingsStore.saveNow()) showToast("Save failed - not persisted");
      wifi.restart();
      nav.requestRender();
      break;
    }
    case InputManager::BTN_BACK:
      nav.pop();
      break;
    default:
      break;
  }
}

// --- Captive login (7.2d) ---------------------------------------------------

// Auto-login prompt for a network whose cached state says a login page
// waits. Confirm runs the attempt (busy state until the verdict arrives);
// Back leaves the link connected without logging in — the attempt never
// re-prompts within the same link session (the service gates it).
void renderCaptiveLogin(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  drawHeader(target, device, "Portal login");
  const int16_t mid = static_cast<int16_t>(device.height / 2);
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);

  if (portalLogin.running()) {
    drawBodyLine(target, device, mid, "Contacting the login page...");
    drawPassiveNote(target, device, "This can take up to a minute");
    setButtonHints({});
    return;
  }

  const settings::Network* active = settings::activeNetwork(settingsStore.settings());
  char line[64];
  snprintf(line, sizeof(line), "Login page found for %s", active != nullptr ? active->ssid : "this network");
  drawBodyLine(target, device, static_cast<int16_t>(mid - lh), line);
  drawBodyLine(target, device, mid, "Connect automatically?");
  drawPassiveNote(target, device, "Or open the portal page in a browser");
  setButtonHints({.back = "Later", .confirm = "Try"});
}

void captiveLoginButton(Navigation& nav, uint8_t button) {
  if (portalLogin.running()) {
    return; // everything ignored until the verdict edge pops the screen
  }
  switch (button) {
    case InputManager::BTN_CONFIRM:
      if (portalLogin.start()) {
        nav.requestRender(); // flip to the busy frame
      } else {
        showToast("Could not start login");
        nav.pop();
      }
      break;
    case InputManager::BTN_BACK:
      nav.pop(); // "Later": connected but without internet for this session
      break;
    default:
      break;
  }
}

void renderAbout(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  drawHeader(target, device, "About");
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
  const int16_t top = static_cast<int16_t>(kMargin + target.lineHeight(ui::FONT_SLOT_TITLE) + 16);
  drawBodyLine(target, device, top, "Freedea");

  char versionLine[32];
  snprintf(versionLine, sizeof(versionLine), "v%s", kAppVersion);
  drawBodyLine(target, device, static_cast<int16_t>(top + lh + 6), versionLine);

  char heapLine[32];
  snprintf(heapLine, sizeof(heapLine), "Free heap: %lu", ESP.getFreeHeap());
  drawBodyLine(target, device, static_cast<int16_t>(top + 2 * (lh + 6)), heapLine);

  // Licenses entry (6.4): the one action here, so it is drawn highlighted to
  // mark it actionable; Confirm opens the dependency list.
  const ui::Rect row{kMargin, static_cast<int16_t>(top + 2 * (lh + 6) + lh + 18),
                     static_cast<int16_t>(device.width - 2 * kMargin), 56};
  drawMenuRow(target, row, "Licenses", nullptr, true);

  setButtonHints({.back = "Back", .confirm = "Licenses"});
}

void aboutButton(Navigation& nav, uint8_t button) {
  if (button == InputManager::BTN_CONFIRM) {
    nav.push(ScreenId::Licenses);
  } else if (button == InputManager::BTN_BACK) {
    nav.pop();
  }
}

// --- Licenses (6.4) ------------------------------------------------------------

// The full license texts ship in flash (LicenseTexts.h, generated by
// bin/gen_license_texts.py) rather than SD or an on-demand fetch: attribution
// must work offline and without a card, and a TLS fetch handshake peaks at
// tens of KB DRAM (the 6.2 TLS trade-off). constexpr char[] lands in DROM —
// zero DRAM, read through the cache.
struct LicenseEntry {
  const char* name; // menu label
  const char* spdx; // right-aligned SPDX hint
  const char* text; // full license text (DROM)
};

// SPDX ids verified 2026-09-09 against the bundled or upstream license files.
// ESP-IDF and mbedTLS share one Apache-2.0 copy (mbedTLS is dual-licensed and
// we use the Apache option); midea-msmart and climapilot are protocol
// references, not linked components — climapilot publishes no license, see
// its embedded notice.
constexpr LicenseEntry kLicenseEntries[] = {
    {"Freedea", "MIT", lic::kFreedea},
    {"ESP-IDF", "Apache-2.0", lic::kApache20},
    {"Arduino ESP32 core", "LGPL-2.1", lic::kArduinoLgpl},
    {"FreeInk SDK", "MIT", lic::kFreeink},
    {"qrcodegen", "MIT", lic::kQrcodegen},
    {"mbedTLS", "Apache-2.0", lic::kMbedtls},
    {"lwIP", "BSD-3-Clause", lic::kLwipBsd},
    {"WireGuard-ESP32", "BSD-3-Clause", lic::kWireGuardBsd3},
    // Control-row icon glyphs (9.1): ISC, with the Feather-derived MIT subset
    // listed inside the text (lock, monitor, moon, target).
    {"Lucide icons", "ISC + MIT", lic::kLucideIcons},
    {"midea-msmart", "MIT", lic::kMideaMsmart},
    {"climapilot", "ref. only", lic::kClimapilotNotice},
};
constexpr uint8_t kLicenseEntryCount = sizeof(kLicenseEntries) / sizeof(kLicenseEntries[0]);

// Menu selection/scroll for the list, page scroll for the text viewer. The
// MaxScroll/PageLines values are set by the render passes and consumed by the
// buttons.
uint8_t sLicensesSelection = 0;
uint8_t sLicensesScroll = 0;
uint8_t sLicensesMaxScroll = 0;
uint8_t sLicenseTextIndex = 0;
uint16_t sLicenseTextScroll = 0;
uint16_t sLicenseTextMaxScroll = 0;
uint8_t sLicenseTextPageLines = 0;

void renderLicenses(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  drawHeader(target, device, "Licenses");

  constexpr int16_t kRowGap = 12;
  constexpr int16_t kRowHeight = 56;
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
  const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);
  const int16_t rowsTop = static_cast<int16_t>(kMargin + lh + 20);
  const int16_t hardBottom = static_cast<int16_t>(device.height - kMargin - smallLh - 8);
  const int32_t fitAll = (hardBottom - rowsTop + kRowGap) / (kRowHeight + kRowGap);
  const bool scrollable = fitAll < static_cast<int16_t>(kLicenseEntryCount);
  const int16_t rowsBottom = static_cast<int16_t>(hardBottom - (scrollable ? smallLh + 6 : 0));
  int32_t fit = (rowsBottom - rowsTop + kRowGap) / (kRowHeight + kRowGap);
  if (fit < 1) fit = 1;
  const uint8_t visibleRows =
      static_cast<uint8_t>(fit > static_cast<int32_t>(kLicenseEntryCount) ? kLicenseEntryCount : fit);
  sLicensesMaxScroll = static_cast<uint8_t>(kLicenseEntryCount > visibleRows ? kLicenseEntryCount - visibleRows : 0);
  if (sLicensesScroll > sLicensesMaxScroll) sLicensesScroll = sLicensesMaxScroll;
  // Keep the selection inside the visible window.
  if (sLicensesSelection < sLicensesScroll) {
    sLicensesScroll = sLicensesSelection;
  } else if (static_cast<uint16_t>(sLicensesSelection) + 1 > static_cast<uint16_t>(sLicensesScroll) + visibleRows) {
    sLicensesScroll = static_cast<uint8_t>(sLicensesSelection + 1 - visibleRows);
  }

  for (uint8_t i = 0; i < visibleRows; ++i) {
    const uint8_t idx = static_cast<uint8_t>(sLicensesScroll + i);
    if (idx >= kLicenseEntryCount) break;
    const ui::Rect rowRect{kMargin, static_cast<int16_t>(rowsTop + i * (kRowHeight + kRowGap)),
                           static_cast<int16_t>(device.width - 2 * kMargin), kRowHeight};
    drawMenuRow(target, rowRect, kLicenseEntries[idx].name, kLicenseEntries[idx].spdx, idx == sLicensesSelection);
  }

  if (scrollable) {
    ui::TextStyle hintText{};
    hintText.font = ui::FONT_SLOT_SMALL;
    hintText.align = ui::TextAlign::Center;
    char pos[24];
    snprintf(pos, sizeof(pos), "rows %u-%u of %u", static_cast<unsigned>(sLicensesScroll + 1),
             static_cast<unsigned>(sLicensesScroll + visibleRows), static_cast<unsigned>(kLicenseEntryCount));
    target.text(ui::Rect{0, static_cast<int16_t>(rowsBottom + 6), device.width, smallLh}, pos, hintText);
  }

  setButtonHints({.back = "Back", .confirm = "View", .up = "Up", .down = "Down"});
}

void licensesButton(Navigation& nav, uint8_t button) {
  aliasSideToNav(button);
  switch (button) {
    case InputManager::BTN_UP:
      if (sLicensesSelection > 0) {
        --sLicensesSelection;
        nav.requestRender();
      }
      break;
    case InputManager::BTN_DOWN:
      if (sLicensesSelection + 1 < kLicenseEntryCount) {
        ++sLicensesSelection;
        nav.requestRender();
      }
      break;
    case InputManager::BTN_CONFIRM:
      sLicenseTextIndex = sLicensesSelection;
      sLicenseTextScroll = 0;
      nav.push(ScreenId::LicenseText);
      break;
    case InputManager::BTN_BACK:
      nav.pop();
      break;
    default:
      break;
  }
}

// Word-wrapped viewer for one license text. Display lines are computed on the
// fly from the flash constant: the wrap scan is negligible against the e-ink
// draw, and a precomputed line table would only pay off across orientations.
void renderLicenseText(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  const LicenseEntry& entry = kLicenseEntries[sLicenseTextIndex];
  drawHeader(target, device, entry.name);

  const ui::FontId compactFont = freedea::kFontSlotCompact;
  ui::TextStyle textStyle{};
  textStyle.font = compactFont;
  const int16_t lineH = target.lineHeight(compactFont);
  const int16_t bodyLh = target.lineHeight(ui::FONT_SLOT_BODY);
  const int16_t smallLh = target.lineHeight(ui::FONT_SLOT_SMALL);
  const int16_t textTop = static_cast<int16_t>(kMargin + bodyLh + 16);
  const int16_t hardBottom = static_cast<int16_t>(device.height - kMargin - smallLh - 8);
  const int16_t textBottom = static_cast<int16_t>(hardBottom - smallLh - 6);
  const int16_t textW = static_cast<int16_t>(device.width - 2 * kMargin);
  int32_t visible = (textBottom - textTop) / lineH;
  if (visible < 1) visible = 1;
  sLicenseTextPageLines = static_cast<uint8_t>(visible);
  // Clamp against the previous frame's bound so the drawn window can never
  // fall past the text; the fresh bound below narrows it further.
  if (sLicenseTextScroll > sLicenseTextMaxScroll) sLicenseTextScroll = sLicenseTextMaxScroll;

  // Wrap scratch on the render path: one display line + one word (stack
  // budget: ~180 B).
  char lineBuf[128];
  char wordBuf[48];
  const int16_t spaceW = target.measureText(compactFont, " ", textStyle).width;

  // Emit one wrapped display line: counted always, drawn only when inside
  // the [scroll, scroll+visible) window.
  uint16_t lineIdx = 0;
  const auto flushLine = [&](int16_t len) {
    if (lineIdx >= sLicenseTextScroll &&
        static_cast<int32_t>(lineIdx) < static_cast<int32_t>(sLicenseTextScroll) + visible) {
      const int16_t y = static_cast<int16_t>(textTop + (lineIdx - sLicenseTextScroll) * lineH);
      if (len > 0) target.text(ui::Rect{kMargin, y, textW, lineH}, lineBuf, textStyle);
    }
    ++lineIdx;
  };

  const char* p = entry.text;
  const char* end = p + std::strlen(p);
  while (p < end) {
    const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
    if (!nl) nl = end;
    size_t lineLen = static_cast<size_t>(nl - p);
    while (lineLen > 0 && p[lineLen - 1] == '\r')
      --lineLen;

    // Preserve leading indentation (license clauses hang on it) on the first
    // display line of this source line only.
    size_t indent = 0;
    while (indent < lineLen && p[indent] == ' ')
      ++indent;
    int16_t curLen = 0;
    bool hasWord = false;
    if (indent > 0 && indent < 12) {
      memcpy(lineBuf, p, indent);
      curLen = static_cast<int16_t>(indent);
      lineBuf[curLen] = '\0';
    }

    size_t pos = indent;
    while (pos < lineLen) {
      const char c = p[pos];
      if (c == ' ' || c == '\t' || c == '\r') {
        ++pos;
        continue;
      }
      size_t ws = pos;
      while (pos < lineLen && p[pos] != ' ' && p[pos] != '\t' && p[pos] != '\r')
        ++pos;
      size_t wlen = pos - ws;
      if (wlen > sizeof(wordBuf) - 1) {
        wlen = sizeof(wordBuf) - 1;
        pos = ws + wlen;
      }
      memcpy(wordBuf, p + ws, wlen);
      wordBuf[wlen] = '\0';
      const int16_t wordW = target.measureText(compactFont, wordBuf, textStyle).width;
      const int16_t addW = wordW + (hasWord ? spaceW : 0);
      if (curLen > 0 && (target.measureText(compactFont, lineBuf, textStyle).width + addW) > textW) {
        flushLine(curLen);
        curLen = 0;
        hasWord = false;
      }
      if (static_cast<size_t>(curLen) + 1 + wlen >= sizeof(lineBuf)) {
        // Longer than the display buffer: break the line here.
        flushLine(curLen);
        curLen = 0;
        hasWord = false;
      }
      if (hasWord) lineBuf[curLen++] = ' ';
      memcpy(lineBuf + curLen, wordBuf, wlen);
      curLen += static_cast<int16_t>(wlen);
      lineBuf[curLen] = '\0';
      hasWord = true;
    }
    flushLine(curLen); // empty source lines count as blank lines
    p = nl + 1;
  }

  const uint16_t totalLines = lineIdx;
  sLicenseTextMaxScroll = static_cast<uint16_t>(totalLines > visible ? totalLines - static_cast<uint16_t>(visible) : 0);
  if (sLicenseTextScroll > sLicenseTextMaxScroll) sLicenseTextScroll = sLicenseTextMaxScroll;

  if (totalLines > 1) {
    ui::TextStyle hintText{};
    hintText.font = ui::FONT_SLOT_SMALL;
    hintText.align = ui::TextAlign::Center;
    char posBuf[32];
    const uint16_t first = static_cast<uint16_t>(sLicenseTextScroll + 1);
    const uint16_t last =
        static_cast<uint16_t>(totalLines < static_cast<uint32_t>(sLicenseTextScroll) + static_cast<uint32_t>(visible)
                                  ? totalLines
                                  : sLicenseTextScroll + static_cast<uint16_t>(visible));
    snprintf(posBuf, sizeof(posBuf), "lines %u-%u of %u", first, last, totalLines);
    target.text(ui::Rect{0, static_cast<int16_t>(textBottom + 6), device.width, smallLh}, posBuf, hintText);
  }

  setButtonHints({.back = "Back", .up = "Up", .down = "Down"});
}

void licenseTextButton(Navigation& nav, uint8_t button) {
  aliasSideToNav(button);
  const uint16_t page = sLicenseTextPageLines ? sLicenseTextPageLines : 1;
  switch (button) {
    case InputManager::BTN_UP:
      if (sLicenseTextScroll > 0) {
        sLicenseTextScroll = sLicenseTextScroll >= page ? static_cast<uint16_t>(sLicenseTextScroll - page) : 0;
        nav.requestRender();
      }
      break;
    case InputManager::BTN_DOWN:
      if (sLicenseTextScroll < sLicenseTextMaxScroll) {
        const uint32_t next = static_cast<uint32_t>(sLicenseTextScroll) + page;
        sLicenseTextScroll = next > sLicenseTextMaxScroll ? sLicenseTextMaxScroll : static_cast<uint16_t>(next);
        nav.requestRender();
      }
      break;
    case InputManager::BTN_BACK:
      nav.pop();
      break;
    default:
      break;
  }
}

// --- Hotspot (provisioning boot) ---------------------------------------------

// Encode scratch for QR version <= 6 (the ~50-char Wi-Fi payload lands in v4).
// Static: two 211-byte buffers would not fit the loop-task stack, and
// rendering only ever runs on the main loop.
constexpr uint8_t kQrMaxVersion = 6;
uint8_t sQrBuf[qrcodegen_BUFFER_LEN_FOR_VERSION(kQrMaxVersion)];
uint8_t sQrTemp[qrcodegen_BUFFER_LEN_FOR_VERSION(kQrMaxVersion)];

void renderHotspot(ui::DisplayTarget& target, const ui::DeviceContext& device, Navigation&) {
  drawHeader(target, device, "Hotspot");

  if (!hotspot.active()) {
    drawBodyLine(target, device, static_cast<int16_t>(device.height / 2), "Hotspot failed to start");
    drawPassiveNote(target, device, "Power-cycle to retry");
    return;
  }

  // Wi-Fi Easy Connect payload: scannable by Android/iOS joiners.
  char payload[64];
  snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;H:false;;", hotspot.ssid(), hotspot.password());

  constexpr int16_t kModule = 8; // px per QR module
  constexpr int16_t kQuiet = 4;  // spec quiet zone, in modules
  const bool encoded = qrcodegen_encodeText(payload, sQrTemp, sQrBuf, qrcodegen_Ecc_MEDIUM, 1, kQrMaxVersion,
                                            qrcodegen_Mask_AUTO, /*boostEcl=*/false);
  const int16_t size = qrcodegen_getSize(sQrBuf);

  int16_t y = static_cast<int16_t>(kMargin + target.lineHeight(ui::FONT_SLOT_BODY) + 16);
  if (encoded && size > 0) {
    const int16_t qrPx = static_cast<int16_t>((size + 2 * kQuiet) * kModule);
    const int16_t x0 = static_cast<int16_t>((device.width - qrPx) / 2);
    const ui::Paint ink = ui::Paint::solid(ui::Color::Black);
    for (int16_t my = 0; my < size; my++) {
      for (int16_t mx = 0; mx < size; mx++) {
        if (!qrcodegen_getModule(sQrBuf, mx, my)) continue;
        target.fill(ui::Rect{static_cast<int16_t>(x0 + (mx + kQuiet) * kModule),
                             static_cast<int16_t>(y + (my + kQuiet) * kModule), kModule, kModule},
                    ink);
      }
    }
    y = static_cast<int16_t>(y + qrPx + 16);
  } else {
    Serial.println("[ui] hotspot QR encode failed");
  }

  const int16_t lh = target.lineHeight(ui::FONT_SLOT_BODY);
  char line[48];
  snprintf(line, sizeof(line), "SSID: %s", hotspot.ssid());
  drawBodyLine(target, device, y, line);
  snprintf(line, sizeof(line), "Password: %s", hotspot.password());
  drawBodyLine(target, device, static_cast<int16_t>(y + lh), line);

  char url[32];
  snprintf(url, sizeof(url), "http://%s", hotspot.ipString());
  drawBodyLine(target, device, static_cast<int16_t>(y + 2 * lh), url);

  // Second, smaller QR carrying just the portal URL: scanning it opens the
  // page directly. Reuses the encode scratch — the join QR above already
  // lives in the framebuffer as pixels.
  y = static_cast<int16_t>(y + 3 * lh + 16);
  if (qrcodegen_encodeText(url, sQrTemp, sQrBuf, qrcodegen_Ecc_MEDIUM, 1, kQrMaxVersion, qrcodegen_Mask_AUTO,
                           /*boostEcl=*/false)) {
    constexpr int16_t kUrlModule = 4;
    const int16_t urlSize = qrcodegen_getSize(sQrBuf);
    const int16_t urlPx = static_cast<int16_t>((urlSize + 2 * kQuiet) * kUrlModule);
    const int16_t x0 = static_cast<int16_t>((device.width - urlPx) / 2);
    const ui::Paint ink = ui::Paint::solid(ui::Color::Black);
    for (int16_t my = 0; my < urlSize; my++) {
      for (int16_t mx = 0; mx < urlSize; mx++) {
        if (!qrcodegen_getModule(sQrBuf, mx, my)) continue;
        target.fill(ui::Rect{static_cast<int16_t>(x0 + (mx + kQuiet) * kUrlModule),
                             static_cast<int16_t>(y + (my + kQuiet) * kUrlModule), kUrlModule, kUrlModule},
                    ink);
      }
    }
  }

  drawPassiveNote(target, device, "Open the URL to configure");
  setButtonHints({.back = "Reboot"});
}

// Back on a provisioning boot means "leave": reboot into a normal boot. The
// provision flag is already consumed, and if the stored credentials are still
// broken, Settings -> Hotspot enters provisioning again — no lockout.
void hotspotButton(Navigation&, uint8_t button) {
  if (button == InputManager::BTN_BACK) {
    Serial.println("[ui] hotspot screen: back pressed, rebooting");
    ESP.restart();
  }
}

} // namespace

void setButtonHints(const ButtonHints& hints) {
  sButtonHints = hints;
}

void drawButtonHints(ui::DisplayTarget& target, const ui::DeviceContext& device) {
  // Slots are the physical bar HALVES: Back|Confirm on the bottom-left bar,
  // Left|Right (or Up|Down where the screen aliases side navigation onto
  // that bar) on the bottom-right bar. A chip is anchored to its own half,
  // so a lone chip still sits over its switch — never centered over the
  // whole bar (crosspoint pattern, user 2026-09-09).
  const char* const navLeft = sButtonHints.left != nullptr ? sButtonHints.left : sButtonHints.up;
  const char* const navRight = sButtonHints.right != nullptr ? sButtonHints.right : sButtonHints.down;
  const char* const labels[] = {sButtonHints.back, sButtonHints.confirm, navLeft, navRight};
  constexpr int8_t kBar[] = {0, 0, 1, 1};
  constexpr uint8_t kChipCount = 4;
  constexpr uint8_t kBarMarginPercent = 8; // screen edge to outer bar end (device photos)
  constexpr int16_t kBarCenterGap = 10;    // gap between the two bars
  constexpr int16_t kChipGap = 10;         // gap between a bar's two chips
  constexpr int16_t kChipMaxW = 110;       // landscape cap (bar alignment is portrait-only)
  constexpr uint8_t kChipRadius = 7;

  const ui::FontId font = freedea::kFontSlotCompact;
  const int16_t lh = target.lineHeight(font);
  const int16_t edge = static_cast<int16_t>(device.width * kBarMarginPercent / 100);
  const int16_t barW = static_cast<int16_t>((device.width - 2 * edge - kBarCenterGap) / 2);
  const int16_t halfW = static_cast<int16_t>(barW / 2);
  int16_t chipW = static_cast<int16_t>(halfW - kChipGap); // fixed width across all screens
  if (chipW > kChipMaxW) chipW = kChipMaxW;
  if (halfW <= 0 || chipW <= 0) return;

  ui::TextStyle style{};
  style.font = font;
  style.align = ui::TextAlign::Center;

  const int16_t chipH = buttonHintsBarHeight(target);
  const int16_t y = buttonHintsTop(target, device);
  const bool flush = sToastText[0] == '\0'; // lifted chips keep the closed bottom
  const ui::Paint ink = ui::Paint::solid(ui::Color::Black);
  const int16_t barLeft[2] = {edge, static_cast<int16_t>(device.width - edge - barW)};
  for (uint8_t i = 0; i < kChipCount; ++i) {
    if (labels[i] == nullptr) continue;
    const int16_t x = static_cast<int16_t>(barLeft[kBar[i]] + (i & 1) * halfW + (halfW - chipW) / 2);
    target.stroke(ui::Rect{x, y, chipW, chipH}, ink, 1, kChipRadius);
    if (flush) {
      // Open bottom: erase the chip's bottom border row and let the screen
      // edge act as its border.
      target.fill(ui::Rect{x, static_cast<int16_t>(y + chipH - 1), chipW, 1}, ui::Paint::solid(ui::Color::White));
    }
    target.text(ui::Rect{x, static_cast<int16_t>(y + (chipH - lh) / 2), chipW, lh}, labels[i], style);
  }
}

// Main-loop hook after AcService publishes a changed state: reseed the
// Control screen's composition base so any pending edits restart from the
// device's acknowledged truth.
void onAcStateChanged() {
  sControlDesired = acService.snapshot();
}

bool controlDropdownOpen() {
  return sDropdownOpen;
}

// Error toast (5.2): latest message wins, shown as an inverted strip in the
// bottom margin while a button hint bar (if any) rides above it. State lives
// with the hint bar near the top of this file.
void showToast(const char* message) {
  snprintf(sToastText, sizeof(sToastText), "%s", message);
  sToastUntilMs = millis() + kToastMs;
}

bool consumeToastExpiry() {
  if (sToastText[0] != '\0' && static_cast<uint32_t>(millis() - sToastUntilMs) >= kToastMs) {
    sToastText[0] = '\0';
    return true;
  }
  return false;
}

// Battery indicator (5.4.3): latest percentage sampled by the main loop
// (0xFFFF = none yet, label hidden). The X4 is ADC-only — voltage off the
// discharge curve, no charging detection — so it renders as a plain "N%".
static uint16_t sBatteryPct = 0xFFFF;

void setBatteryPercent(uint16_t percent) {
  sBatteryPct = percent;
}

// Global connectivity banner (5.1), drawn by Navigation over every screen
// while the STA link is down: WifiService's capped backoff reconnects on its
// own, but the user must see why AC data stopped. kDisabled never shows it —
// on provisioning boots (and SSID-less configs) the radio is off by design.
// The filled strip lives inside the top kMargin every screen reserves, so it
// never covers a header. The error toast (5.2) rides the same call: its
// bottom strip draws independently of the link state.
void drawStatusBanner(ui::DisplayTarget& target, const ui::DeviceContext& device) {
  const int16_t lh = target.lineHeight(ui::FONT_SLOT_SMALL);
  const int16_t barH = static_cast<int16_t>(lh + 4 > kMargin ? kMargin : lh + 4);
  ui::TextStyle style{};
  style.font = ui::FONT_SLOT_SMALL;
  style.align = ui::TextAlign::Center;
  style.inverted = true;

  const WifiService::Link link = wifi.link();
  const bool bannerUp = link == WifiService::Link::kConnecting || link == WifiService::Link::kDisconnected;

  // Top strip: WiFi link banner (5.1).
  if (bannerUp) {
    const char* text = link == WifiService::Link::kConnecting ? "WiFi connecting..." : "WiFi lost - reconnecting...";
    target.fill(ui::Rect{0, 0, device.width, barH}, ui::Paint::solid(ui::Color::Black));
    target.text(ui::Rect{0, static_cast<int16_t>((barH - lh) / 2), device.width, lh}, text, style);
  }
  // Bottom strip: error toast (5.2), independent of the link banner.
  if (sToastText[0] != '\0') {
    target.fill(ui::Rect{0, static_cast<int16_t>(device.height - barH), device.width, barH},
                ui::Paint::solid(ui::Color::Black));
    target.text(ui::Rect{0, static_cast<int16_t>(device.height - barH + (barH - lh) / 2), device.width, lh}, sToastText,
                style);
  }
  // Battery percentage (5.4.3): top-right corner, clear of the left-aligned
  // titles on the same line. While the WiFi strip covers that corner it rides
  // inside the strip, inverted with the banner text.
  if (sBatteryPct <= 100) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", static_cast<unsigned>(sBatteryPct));
    ui::TextStyle bat{};
    bat.font = ui::FONT_SLOT_SMALL;
    bat.align = ui::TextAlign::Right;
    int16_t y = static_cast<int16_t>(kMargin + (target.lineHeight(ui::FONT_SLOT_BODY) - lh) / 2);
    if (bannerUp) {
      bat.inverted = true;
      y = static_cast<int16_t>((barH - lh) / 2);
    }
    target.text(ui::Rect{0, y, static_cast<int16_t>(device.width - kMargin), lh}, buf, bat);
  }
}

const ScreenHandlers& screenFor(ScreenId id) {
  static constexpr ScreenHandlers kHandlers[] = {
      {"Freedea", renderHome, homeButton},
      {"Dashboard", renderDashboard, dashboardButton},
      {"Control", renderControl, controlButton},
      {"Settings", renderSettings, settingsButton},
      {"About", renderAbout, aboutButton},
      {"Hotspot", renderHotspot, hotspotButton},
      {"Devices", renderDevices, devicesButton},
      {"Details", renderDetails, detailsButton},
      {"Licenses", renderLicenses, licensesButton},
      {"License text", renderLicenseText, licenseTextButton},
      {"WiFi networks", renderNetworks, networksButton},
      {"Portal login", renderCaptiveLogin, captiveLoginButton},
  };
  static_assert(sizeof(kHandlers) / sizeof(kHandlers[0]) == static_cast<size_t>(ScreenId::CaptiveLogin) + 1,
                "registry must cover every ScreenId");
  const uint8_t index = static_cast<uint8_t>(id);
  return index < sizeof(kHandlers) / sizeof(kHandlers[0]) ? kHandlers[index] : kHandlers[0];
}
