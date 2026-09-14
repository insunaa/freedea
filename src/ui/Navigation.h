#pragma once

#include <stdint.h>

#include <EInkDisplay.h>

#include "Screens.h"

class InputManager;

// Drives the screen registry: fixed-capacity back stack, button routing
// (Up/Down/Left/Right/Confirm/Back), and e-ink refresh hygiene — every screen
// draw goes through renderCurrent(): FAST_REFRESH, promoted to HALF_REFRESH
// every kHalfRefreshEvery-th draw; FULL_REFRESH only on an explicit
// requestFullRefresh() (power-button tap), never on a timer, so an idle
// screen never deep-refreshes. The power button is system-wide: tap = that
// clean refresh, >= 3 s hold = standby (consumePowerOff, handled by main).
// 9.2 discipline on top of that: kIdleRefreshMs without a button press
// re-presents the held frame with a FULL refresh — the deep waveform
// re-drives every pixel and equalizes the retention a static layout biases —
// repeating every kIdleRefreshMs of continued stillness. No-op repaint
// suppression is value-based at the sources (services raise edges only on
// changed values, e.g. the AcService group-round edge), never by comparing
// rendered framebuffers: the 48 KB diff buffer that would require was
// rejected as a RAM cost.
class Navigation {
public:
  static constexpr uint8_t kBackstackCapacity = 8;
  static constexpr uint8_t kHalfRefreshEvery = 30;
  static constexpr uint32_t kIdleRefreshMs = 300000; // 5 min without input -> FULL refresh

  Navigation(EInkDisplay& display, InputManager& input);

  // Make `root` the current screen and draw it (full refresh); the back
  // stack below it is empty, so Back is a no-op on the root screen.
  void start(ScreenId root = ScreenId::Home);
  // Drain every queued press (async input task), dispatch, then render once
  // if anything marked dirty; also repaints on service requestRender().
  void tick();

  void push(ScreenId screen);
  void pop(); // no-op at the root screen
  ScreenId current() const;
  void requestRender() { dirty_ = true; }
  // Hook called once per drained press burst, before dispatch and render, so
  // the frame the press triggered rasterizes at full CPU clock (the main
  // loop's applyCpuPolicy runs after the draw; the measured cost of restoring
  // late was ~+280 ms on the first press after the idle downclock).
  void setActivityHook(void (*hook)(bool)) { activityHook_ = hook; }
  // Held-frame path: a screen may modify the displayed framebuffer in place
  // through heldFrame() and push it with presentHeldFrame(), skipping the
  // full-screen rasterize (the Control dropdown's highlight blit). Both are
  // main-loop only; the caller must leave the frame in exactly the state a
  // re-render would produce, so any later full draw is consistent.
  freeink::ui::DisplayTarget heldFrame();
  void presentHeldFrame();
  // Repaint the current screen with the deep FULL waveform once (the X4
  // power button's tap): the user's on-demand ghosting clear, in place of a
  // periodic full refresh.
  void requestFullRefresh() {
    dirty_ = true;
    forceFull_ = true;
  }

  // True once after a tick drained at least one button press; consumed by the
  // main loop's CPU frequency policy (5.4) to tell active from idle time.
  bool consumeActivity() {
    const bool activity = activity_;
    activity_ = false;
    return activity;
  }

  // True once after the power button was held at least kPowerOffHoldMs and
  // released — the system standby gesture (5.4). The main loop draws the
  // power-off frame and cuts the battery latch.
  bool consumePowerOff() {
    const bool off = powerOff_;
    powerOff_ = false;
    return off;
  }

private:
  static constexpr uint32_t kPowerOffHoldMs = 3000; // power hold -> standby

  void renderCurrent();
  void presentFrame(uint32_t rasterStartUs); // ladder + blocking push (raster 0 = held frame)

  EInkDisplay& display_;
  InputManager& input_;
  ScreenId backstack_[kBackstackCapacity] = {ScreenId::Home};
  uint8_t depth_ = 0;
  uint8_t fastRefreshes_ = 0; // fast draws since the last half refresh
  bool forceFull_ = false;    // next draw uses FULL_REFRESH (one-shot)
  bool dirty_ = false;
  bool activity_ = false;          // a press was drained during the last tick
  bool powerPressPending_ = false; // BTN_POWER down, gesture undecided
  uint32_t powerPressAtMs_ = 0;    // when BTN_POWER was first observed down
  bool powerOff_ = false;          // long power hold latched (consumed by main)
  uint32_t lastInputMs_ = 0;       // last drained press (idle-refresh clock, re-armed on fire)
  uint32_t pressAtMs_ = 0;         // first press of the drained burst (latency probe)
  void (*activityHook_)(bool) = nullptr;
};
