#include "Navigation.h"

#include <Arduino.h>

#include <EInkDisplay.h>
#include <FreeInkUIDisplayTarget.h>
#include <InputManager.h>

#include "Screens.h"
#include "UiFonts.h"
#include "fonts/NotoSans14Font.h"

namespace ui = freeink::ui;

Navigation::Navigation(EInkDisplay& display, InputManager& input) : display_(display), input_(input) {}

ScreenId Navigation::current() const {
  return backstack_[depth_];
}

void Navigation::start(ScreenId root) {
  depth_ = 0;
  backstack_[0] = root;
  fastRefreshes_ = 0;
  forceFull_ = false;
  lastInputMs_ = millis();
  renderCurrent();
}

void Navigation::push(ScreenId screen) {
  if (depth_ + 1 >= kBackstackCapacity) {
    Serial.printf("[nav] backstack full, ignoring push\n");
    return;
  }
  backstack_[++depth_] = screen;
  dirty_ = true;
}

void Navigation::pop() {
  if (depth_ == 0) return;
  --depth_;
  dirty_ = true;
}

void Navigation::tick() {
  // Presses arrive through the async input task's queue (beginAsync in
  // setup): taps that land during a blocking e-ink refresh survive as
  // individual events instead of being sampled away. Drain the whole queue so
  // a burst advances as many steps — each press routes against the screen it
  // lands on, mirroring post-refresh taps — then render once (the handlers'
  // requestRender() calls coalesce through dirty_).
  uint8_t button = 0;
  while (input_.popPress(button)) {
    activity_ = true;
    if (pressAtMs_ == 0) {
      pressAtMs_ = millis(); // latency probe: first press of the burst
      if (activityHook_) activityHook_(true);
    }
    lastInputMs_ = millis();
    // The power button is a system gesture, never routed to screens. Its
    // outcome is decided below on release (short = clean refresh) or on the
    // 3 s hold (standby), so here we only latch the press edge.
    if (button == InputManager::BTN_POWER) {
      if (!powerPressPending_) {
        powerPressPending_ = true;
        powerPressAtMs_ = millis();
      }
      continue;
    }
    screenFor(current()).onButton(*this, button);
  }

  // Power gesture. isPowerButtonPressed() reflects the async task's live
  // level, so it is valid even while the main loop was blocked in a refresh.
  if (powerPressPending_) {
    const uint32_t heldMs = millis() - powerPressAtMs_;
    if (heldMs >= kPowerOffHoldMs) {
      // Long hold: arm standby without waiting for release. The main loop
      // draws the power-off frame, waits for release, then cuts power.
      powerPressPending_ = false;
      powerOff_ = true;
      return;
    }
    if (!input_.isPowerButtonPressed()) {
      // Released before the hold threshold: the 4.6 on-demand full refresh.
      powerPressPending_ = false;
      requestFullRefresh();
    }
  }

  // Idle refresh (9.2): five minutes without a button press re-presents the
  // held frame through the FULL waveform — every pixel is re-driven, which
  // equalizes the retention a static layout accumulates. The clock re-arms
  // here, so continued stillness repeats it every kIdleRefreshMs; a press
  // resets it too. Service repaints in between stay on the normal ladder.
  if (millis() - lastInputMs_ >= kIdleRefreshMs) {
    lastInputMs_ = millis();
    Serial.println("[NAV ] idle refresh: presenting held frame FULL");
    requestFullRefresh();
  }

  // Also reached without input: requestRender() from a service callback
  // (e.g. a WiFi link transition) must still repaint.
  if (dirty_)
    renderCurrent();
  else
    pressAtMs_ = 0; // press did not schedule a frame; don't bill a later one
}

void Navigation::renderCurrent() {
  dirty_ = false;
  const uint32_t frameStartUs = micros();
  display_.clearScreen();
  ui::DisplayTarget target(display_.getFrameBuffer(), display_.getDisplayWidth(), display_.getDisplayHeight(),
                           display_.getDisplayWidthBytes());
  target.setFont(freedea::kFontSlotCompact, freeink::ui::kNotoSans14Font);
  setButtonHints({});
  screenFor(current()).render(target, target.deviceContext(), *this);
  // 7.1a: shared chip bar, composed from the hints the screen declared this
  // frame — the footer is an emergent property, not per-screen text.
  drawButtonHints(target, target.deviceContext());
  // 5.1: overlay the connectivity banner last so it lands on every screen
  // (inside the top margin, over whatever the screen drew there).
  drawStatusBanner(target, target.deviceContext());

  presentFrame(frameStartUs);
}

freeink::ui::DisplayTarget Navigation::heldFrame() {
  return ui::DisplayTarget(display_.getFrameBuffer(), display_.getDisplayWidth(), display_.getDisplayHeight(),
                           display_.getDisplayWidthBytes());
}

void Navigation::presentHeldFrame() {
  presentFrame(micros());
}

// Ladder + blocking push of the current framebuffer, shared by the full
// render and the held-frame blit path. R.4 latency split: raster (CPU draw
// into the framebuffer) vs. the blocking displayBuffer, per physical refresh
// — the split that localized the dropdown lag to the rasterize, not the
// waveform. "raster 0" marks a held-frame present.
void Navigation::presentFrame(uint32_t rasterStartUs) {
  const bool full = forceFull_;
  forceFull_ = false;
  const bool half = !full && fastRefreshes_ == 0;
  const uint32_t rasterUs = micros();
  display_.displayBuffer(full   ? EInkDisplay::FULL_REFRESH
                         : half ? EInkDisplay::HALF_REFRESH
                                : EInkDisplay::FAST_REFRESH);
  const uint32_t refreshUs = micros() - rasterUs;
  // One line per physical refresh: the observable trail for the 4.6 checks
  // (an idle screen must produce none).
  const uint32_t pressAtMs = pressAtMs_;
  pressAtMs_ = 0;
  Serial.printf("[NAV ] draw %s raster %lu ms refresh %lu ms",
                full   ? "full"
                : half ? "half"
                       : "fast",
                static_cast<unsigned long>((rasterUs - rasterStartUs) / 1000UL),
                static_cast<unsigned long>(refreshUs / 1000UL));
  if (pressAtMs) Serial.printf(" press->draw %lu ms", static_cast<unsigned long>(millis() - pressAtMs));
  Serial.println();
  fastRefreshes_ = static_cast<uint8_t>((fastRefreshes_ + 1) % kHalfRefreshEvery);
}
