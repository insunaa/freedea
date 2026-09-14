#pragma once

// Freedea — background AC service (Phase 4.2 + 4.5 + 4.6 + 4.7 + 5.1 + 5.2).
//
// Owns the AC state cache: a FreeRTOS task keeps a LanTransport session to
// the first usable stored device open (connect + V3 auth, one capabilities
// query, then adaptive GetState polling), applies queued commands, consumes
// the device's unsolicited state pushes (physical-remote changes land in ~1 s
// instead of waiting for the next poll), and publishes updates into a
// mutex-guarded midea::AcState snapshot the UI copies on demand. Sessions
// retry with capped backoff; the UI reads the snapshot regardless.
//
// Polling (4.6, refined by the 6.1c poll-mode policy): the UI tells the
// service which PollMode applies — the screen that actually shows AC data
// decides. kNone (Home/Settings/About/...): zero app-layer traffic; the
// socket stays open on TCP keepalive (LanTransport) and arriving pushes are
// still drained, and if the AC dropped the session the service stays silent
// (no reconnect churn) until an AC screen comes up. kState (Control) and
// kStatePower (Dashboard, + group 7 power) poll every kPollIdleMs; kStateAll
// (Details) polls GetState + one full group round every kPollDetailsMs.
// Entering any polling mode polls immediately ("fresh on navigate").
// A local change (notifyLocalChange(); queueCommand() raises it too) makes
// the task resync immediately and hold kPollFastMs for kFastWindowMs so the
// device's echo of the change arrives within seconds, then decay back to idle
// — and keeps the session alive even in kNone until the window closes.
//
// Control (4.7): queueCommand(kSetState) hands the desired state to the task
// (latest desired wins; the UI composes each command so coalescing is safe).
// As soon as a session is alive the task sends SetState, then immediately
// GetStates so the published snapshot reflects the device's acknowledged
// truth, and opens the fast-poll window. A failed send resyncs anyway: the
// device's truth is the source of truth either way.
//
// Capabilities (4.5): after each successful authentication the task queries
// GetCapabilities (plus the additional page when the device signals one) and
// keeps the merged result for the rest of the boot — devices do not change
// capabilities at runtime. consumeCapabilitiesEdge() lets the main loop
// persist the result to the SD cache without the task touching SdFat.
//
// Until the first state arrives, indoorTemperature stays nullopt, which is
// the UI's "no data yet" sentinel.
//
// Reachability (5.1): sessionUp() mirrors "WiFi link up and transport alive"
// (refreshed each task tick; publishState also raises it), and
// lastStateAgeMs() counts since the last received state — together they let
// the UI show stale data honestly. Reconnect itself is existing machinery:
// WifiService owns link retries with capped backoff, and the transport
// reconnects with its own backoff whenever WiFi holds.
//
// Error surfacing (5.2): protocol-level failures (malformed frames, session
// teardown, failed or unacknowledged sends) land in a latest-wins message
// slot the main loop drains into a UI toast — once per occurrence, never per
// backoff retry, so recovery loops stay quiet. All error paths were already
// serial-logged; capability-query failures deliberately stay log-only (the
// Control screen renders "unknown caps" honestly) to avoid periodic toasts
// when a device never answers the caps query.

#include <cstdint>
#include <optional>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <AcState.h>
#include <Devices.h>
#include <Responses.h>
#include <Settings.h>

#include "LanTransport.h"

class WifiService;

class AcService {
public:
  // A property the UI can toggle on the 0xB0/0xB1 property channel (8.2, 8.5,
  // 8.6, 8.8 add members). The queue carries this tag plus a raw value byte;
  // the service maps it onto the typed SetPropertiesCommand field and its
  // codec.
  enum class PropKind : uint8_t {
    kIeco,
    kOutSilent,
    kSelfClean,
    kBreezeAway,
    kBreezeless,
    kFlash,
    kRateSelect,
    kCascade,
    kFreshAir,
    // BUZZER (0x001A, 8.8): not readable back, so the row is setting-backed
    // and this kind is deliberately not caps-gated on the send.
    kBeep,
    kCount
  };
  static constexpr size_t kPropKindCount = static_cast<size_t>(PropKind::kCount);

  // Acknowledged property-channel snapshot (8.2). Each feature carries a
  // value plus a known bit: known is false until a caps-supported GET returns
  // a value, so the UI hides a row until it has device truth (no optimistic
  // flip). Grows a value byte + known bit per enum feature in 8.6; static, no
  // heap. iECO/out-silent are boolean here.
  struct Props {
    bool ieco = false;
    bool outSilent = false;
    bool iecoKnown = false;
    bool outSilentKnown = false;
    // 8.5 booleans; selfCleanRunning is the one-shot self-clean cycle state.
    bool selfCleanRunning = false;
    bool breezeAway = false;
    bool breezeless = false;
    bool flash = false;
    bool selfCleanKnown = false;
    bool breezeAwayKnown = false;
    bool breezelessKnown = false;
    bool flashKnown = false;
    // 8.6 enum values, protocol-native (rate select gear byte with 100 = off,
    // cascade 0/1/2, fresh-air speed 0/40/60/80/100).
    uint8_t rateSelect = 100;
    uint8_t cascade = 0;
    uint8_t freshAir = 0;
    bool rateSelectKnown = false;
    bool cascadeKnown = false;
    bool freshAirKnown = false;
  };

  // One entry of the command queue. Fixed-size POD (required by FreeRTOS
  // queue copies); kSetState carries the full desired state built by the
  // Control screen from the latest snapshot, kSetProperty carries a property
  // tag + value byte (the desired state field is unused for it), and
  // kToggleDisplay carries nothing (the command only toggles, 8.7).
  struct Command {
    enum class Kind : uint8_t { kSetState, kSetProperty, kToggleDisplay };
    Kind kind;
    midea::AcState desired;
    PropKind propKind = PropKind::kIeco;
    uint8_t propValue = 0;
  };

  // What the currently visible screen needs from the AC link (6.1c). The
  // main loop maps ScreenId -> PollMode every pass; setPollMode is cheap and
  // only the transitions matter.
  enum class PollMode : uint8_t {
    kNone,       // no AC data shown: passive (keepalive socket + pushes only)
    kState,      // GetState on the idle cadence (Control)
    kStatePower, // GetState + group 7 power on the idle cadence (Dashboard)
    kStateAll,   // GetState + full group round every kPollDetailsMs (Details)
  };

  // Selects the first usable record: known IP, and either V3 credentials or
  // version 2. Shared with the main loop (capabilities cache key lookup).
  static const devices::Device* selectTarget(const devices::List& devices);

  // Picks the first usable device from the store (selectTarget), seeds the
  // capabilities from cachedCaps when supplied (may be null), then creates
  // mutex/queue/task. Returns false (and leaves the service inert) if any
  // allocation fails; call once at boot, after the stores and WiFi service.
  // The settings reference stays live: the task reads the single-byte AC
  // flags (beep, 8.8) at send time; the main thread only ever flips bytes.
  bool init(const devices::List& devices, const WifiService& wifi, const settings::Settings& settings,
            const midea::AcCapabilities* cachedCaps = nullptr);

  // Copy of the latest state under the mutex. Before init() succeeds (or
  // before the first poll) this is a default-constructed never-seen state;
  // indoorTemperature == nullopt is the "no data yet" sentinel.
  midea::AcState snapshot() const;

  // Group-data snapshot (6.1): accumulated last-known extended stats written
  // by the polling modes. extGroupsSeen() is a bitmask of answered group
  // numbers (bit N = group N); consumeExtUpdate() fires once per completed
  // poll round whose parsed values differ from the previously announced
  // snapshot (a round of unchanged numbers raises no edge), so the
  // Details/Dashboard screens repaint at most once per round and never on
  // identical data.
  midea::AcExtStats extSnapshot() const;
  uint16_t extGroupsSeen() const;
  // Group-7 real-time power scalar (Dashboard power row): avoids copying the
  // whole ~160 B snapshot into a render stack frame.
  std::optional<uint16_t> outdoorUnitPowerW() const;
  bool consumeExtUpdate();
  // Milliseconds since the last parsed group response (wrap-safe; meaningful
  // only once extGroupsSeen() != 0).
  uint32_t lastExtAgeMs() const;

  // Thread-safe UI hint: sets what the visible screen needs. Transitions
  // into a polling mode make the task poll on its next tick; kNone never
  // triggers traffic (it only lets the task go passive). Safe before init()
  // (stored, applied when the task exists).
  void setPollMode(PollMode mode);

  // True once at least one AC state has been received, even if its fields
  // carry no-data sentinels (e.g. a device that omits the indoor temp).
  bool hasState() const;

  // Reachability (5.1): true while the WiFi link is up and the transport
  // session is alive. The service task refreshes it every tick; the UI pairs
  // it with hasState() for the stale-data indicator.
  bool sessionUp() const;

  // Milliseconds since the last received state (wrap-safe unsigned
  // subtraction; meaningful only once hasState() is true).
  uint32_t lastStateAgeMs() const;

  // True once after a sessionUp() transition (raised by the task, cleared by
  // the read). The main loop consumes it to repaint the stale indicator.
  bool consumeReachabilityEdge();

  // Error surface (5.2): the task records a short user-facing message for
  // protocol-level failures (malformed frames, session teardown, failed
  // sends) into a latest-wins slot; the main loop copies it out here and
  // renders a toast. False when nothing is pending or before init().
  bool consumeError(char* out, size_t cap);

  // True when the service stored a state whose UI-visible fields (power,
  // indoor temperature) differ from the previous one; cleared by the read.
  // The main loop consumes this to repaint the dashboard, so a change on the
  // service task never touches Navigation cross-task.
  bool consumeStateChanged();

  // Copy of the capabilities under the mutex. All-absent flags until the
  // first query lands (or no cache was seeded): callers must treat that as
  // "unknown", never as "unsupported".
  midea::AcCapabilities capabilities() const;

  // True once a real capabilities set is held (queried this boot or seeded
  // from the SD cache).
  bool hasCapabilities() const;

  // True once after capabilities newly arrived from the device (cache seeding
  // does not raise it); cleared by the read. The main loop consumes it to
  // persist the SD cache and repaint gated screens.
  bool consumeCapabilitiesEdge();

  // Copy of the acknowledged property snapshot under the mutex (8.2). Each
  // feature's known bit distinguishes "never queried / caps-denied / GET never
  // answered" (false) from a decoded value; callers hide a row until known.
  Props props() const;

  // True once after any property value arrived and changed the snapshot (get
  // answer or post-set re-query); cleared by the read. Drives the Control
  // rows' repaint (8.3).
  bool consumePropsEdge();

  // Signal that a local change made the cached state stale (the 4.7 control
  // path; safe from the UI task). The service task resyncs on its next tick
  // and holds the fast poll cadence for a window; the caller never blocks on
  // or touches the socket.
  void notifyLocalChange();

  // Device id of the selected target (0 when none). Set at init, never
  // changes while running.
  uint64_t targetId() const { return hasTarget_ ? target_.id : 0; }

  // Queue a command for the service task. The only writer will be the UI;
  // never call from an ISR. False when the queue is full (drops the command).
  // A successful send also raises the resync edge: the cache is stale until
  // the device echoes the change back.
  bool queueCommand(const Command& command);

private:
  static constexpr uint32_t kTaskStackSize = 4096;
  static constexpr UBaseType_t kTaskPriority = 1;
  static constexpr UBaseType_t kQueueDepth = 4;
  // Queue receive timeout: idle cadence of the task and the divisor of the
  // heartbeat below. 1 s keeps command handling and local-change resyncs
  // responsive to the sub-2 s window the fast poll promises; the idle work
  // per tick is a few millis() checks.
  static constexpr uint32_t kTickMs = 1000;
  // Heartbeat: log heap + stack high-water every 30th tick (~30 s).
  static constexpr uint32_t kHeartbeatsPerLog = 30;
  // Response wait for a GetState, post-auth or polled.
  static constexpr uint32_t kResponseTimeoutMs = 3000;
  // Adaptive state poll cadence while the session is alive (4.6), quantized
  // to the 1 s task tick. 5 s: the Porti never pushes state for remote-driven
  // changes (its unsolicited frames are periodic 0xB5 chatter, observed
  // 2026-09-07), so polling is the only discovery path for the physical
  // remote — 5 s bounds remote sync at ~one poll + one draw. Still far under
  // the ~30 s idle-drop devices impose (4.2); the traffic is one small GetState.
  static constexpr uint32_t kPollIdleMs = 5000;
  // Fast window after a local-change resync request, so the device echo of
  // a queued command lands within seconds.
  static constexpr uint32_t kPollFastMs = 2000;
  static constexpr uint32_t kFastWindowMs = 15000;
  // Details mode (6.1c): full state + group round every 3 s (user decision
  // 2026-09-07 — power cost is irrelevant while the Details screen is up).
  static constexpr uint32_t kPollDetailsMs = 3000;
  // Per-group response window inside a poll round; the Porti answers in <100 ms
  // (6.1a spike), so this only bounds dead-group stalls.
  static constexpr uint32_t kGroupResponseMs = 600;
  // Consecutive poll timeouts on a live socket before the session is dropped
  // for a full reconnect + re-auth (a silent device with an open socket must
  // not stay stale forever).
  static constexpr uint8_t kMaxPollTimeouts = 3;
  // Button lock (8.9): minimum gap between two override sends. Detection is
  // poll-paced (5 s), so this mainly guards against reverting faster than the
  // device applies — a pet-mashing remote must not turn the socket into a
  // command flood.
  static constexpr uint32_t kLockRevertMinMs = 5000;
  // Consecutive overrides that did not converge before the lock gives up and
  // reports "Button lock stuck" instead of looping against a device that
  // refuses our writes.
  static constexpr uint8_t kLockMaxStrikes = 3;
  // Reconnect backoff: 5 s doubling to 60 s, reset after a successful open.
  static constexpr uint32_t kFirstBackoffMs = 5000;
  static constexpr uint32_t kMaxBackoffMs = 60000;

  static void taskTrampoline(void* ctx);
  void run();
  // Connect/auth/capabilities/one-GetState state machine, run once per tick.
  void serviceSession();
  void pullStateOnce();
  // Task context: read frames until one decodes as an AC state (absolute
  // deadline). Devices interleave unsolicited frames (pushes, queries), so
  // anything else is skipped and logged with its response id. kOk means a
  // state was published; kLinkDown/kProtocolError need session teardown.
  // logSuffix tags the state line ("", " (ack)").
  LanTransport::Result readStateUntil(uint32_t deadlineMs, const char* logSuffix);
  // Consume a bounded burst of buffered frames between polls so remote-driven
  // changes reach the UI in ~1 s instead of up to one idle poll. False when
  // the session died (already closed). silent=true (passive kNone mode) skips
  // the error toast and backoff: a session that died while no AC screen was
  // shown is a non-event, reopened by the next polling mode.
  bool drainPushes(bool silent = false);
  // Task context: read-modify the requested groups and merge every 0xC1 into
  // the ext_ snapshot (6.1). Each query gets a kGroupResponseMs window; a
  // group that stays silent just costs that window. Interleaved state frames
  // are published; link errors end the round (the next tick recovers).
  void pollGroups(const uint8_t* groups, size_t count);
  // Task context: the extra group polling for the active poll mode after the
  // state poll (none for kState, group 7 for kStatePower, full round for
  // kStateAll).
  void pollModeExtras(PollMode mode);
  // Task context: parse one 0xC1 payload into ext_ under stateMutex_ and
  // raise the seen bit / update edge. False when the group has no parser.
  bool publishExt(uint8_t group, const uint8_t* payload, size_t len);
  // Task context, first thing each tick: copy the mutex-guarded requested
  // mode; on transitions log, and entering a polling mode forces an immediate
  // poll + retry ("fresh on navigate").
  PollMode takePollMode();
  // One-time GetCapabilities (+ additional page when pending). Task context;
  // stores the merged set and raises the persist edge on success.
  void fetchCapabilitiesOnce();
  // Task context, first thing each serviceSession() tick: mirror the link +
  // session health into sessionUp_ and raise reachEdge_ on transitions.
  void updateSessionFlags();
  // Task context: copy a message into the error slot (mutex-guarded,
  // latest-wins). Called once per error occurrence, never per retry, so the
  // UI toast cannot spam the e-ink during backoff loops.
  void reportError(const char* message);
  void scheduleRetry();
  void publishState(const midea::AcState& state);
  // Task context, session alive: send the pending desired state, resync
  // immediately (acknowledged truth), and open the fast-poll window. Clears
  // the pending flag even on failure.
  void sendPendingSetState();
  void sendToggleDisplay();
  // Task context, session bring-up: GET the capabilities-supported properties
  // (all kinds in one query) and merge them into the props snapshot. Returns
  // without sending when caps support none (querying a denied property errors).
  void fetchPropertiesOnce();
  // Task context, session alive: batch every dirty property into one
  // SetPropertiesCommand, log the ack result bits, then re-query for display
  // (acknowledged state only). Clears the dirty set even on failure.
  void sendPendingProps();
  // Task context: read frames until a property response of `want` arrives
  // (publishing interleaved state frames, skipping the rest) or the deadline
  // elapses / the link dies. On kOk, sFrameBuf/`outLen` hold that response.
  LanTransport::Result readUntilPropertyResponse(midea::ResponseKind want, uint32_t deadlineMs, size_t* outLen);
  // Task context: merge decoded property values into props_ under the mutex,
  // raising propsEdge_ only on a visible change (failed TLVs still store).
  void applyProperties(const midea::AcPropertyValues& values);
  // Task-only: current poll interval from the fast window, logging cadence
  // transitions so serial traces show the adaptation.
  uint32_t currentPollIntervalMs();
  // Mutex-guarded read-and-clear of the resync request edge.
  bool consumeResyncEdge();
  // Command beep bit (SetState/ToggleDisplay byte-1 0x40): follows the
  // persisted Sound setting (8.8); true before init or without settings.
  bool beepEnabled() const { return settings_ == nullptr || settings_->ac.beep; }
  // Button lock (8.9): persisted bool read live like the beep flag. Default
  // off (and inert before init): the lock keeps a session + poll alive, so
  // it never engages without an explicit opt-in.
  bool buttonLockEnabled() const { return settings_ != nullptr && settings_->ac.buttonLock != 0; }
  // Task context, every tick: mirror the setting into lockEnabled_, re-arming
  // (invalidate target, clear strikes) on both transitions. Returns the
  // current enabled state so serviceSession() can force the poll floor.
  bool updateButtonLockMode();
  // Task context, session alive and idle: queue an override of an external
  // (button/remote) change back to lockTarget_ — strikes + min-interval
  // loop-breaker per the 8.9 spec. No-op unless armed with no send pending.
  void applyButtonLock();

  mutable SemaphoreHandle_t stateMutex_ = nullptr;
  QueueHandle_t commandQueue_ = nullptr;
  midea::AcState state_;

  // Group-data snapshot (6.1) under stateMutex_: ext_/extShown_/extSeen_/extAtMs_/extEdge_.
  midea::AcExtStats ext_;
  // Byte-identical copy of ext_ as of the last raised extEdge_ (memcpy'd, so
  // the memcmp change gate compares against matching padding bytes).
  midea::AcExtStats extShown_;
  uint16_t extSeen_ = 0;
  uint32_t extAtMs_ = 0;
  bool extEdge_ = false;

  LanTransport transport_;
  devices::Device target_;
  bool hasTarget_ = false;
  const WifiService* wifi_ = nullptr;
  // Live settings reference (8.8): the task reads the byte-sized AC flags at
  // send time; byte loads cannot tear, so no copy is needed for a single bool.
  const settings::Settings* settings_ = nullptr;

  // Capabilities: caps_/capsReceived_/capsEdge_ under stateMutex_; capsKnown_
  // is task-only (and seeded in init() before the task is created), gating
  // the once-per-boot query.
  midea::AcCapabilities caps_;
  bool capsReceived_ = false;
  bool capsEdge_ = false;
  bool capsKnown_ = false;
  // Property snapshot (8.2): props_/propsEdge_ under stateMutex_. Task-only
  // pending set: a newer value for the same kind supersedes an unsent one;
  // distinct dirty kinds batch into one SetPropertiesCommand.
  Props props_;
  bool propsEdge_ = false;
  bool pendingPropDirty_[kPropKindCount] = {};
  uint8_t pendingPropValue_[kPropKindCount] = {};
  bool hasPendingProps_ = false;
  uint32_t retryAtMs_ = 0;
  uint32_t backoffMs_ = kFirstBackoffMs;
  uint32_t nextPollAtMs_ = 0;
  uint32_t fastPollUntilMs_ = 0; // task-only: end of the fast-poll window
  bool fastPollActive_ = false;  // task-only: current cadence, for transition logs
  // Task-only pending control target: the queue drains into these two (a
  // newer command supersedes an unsent one; each desired state is complete).
  midea::AcState pendingDesired_;
  bool hasPendingSetState_ = false;
  bool hasPendingToggleDisplay_ = false; // task-only: coalesces queued toggles
  // Button lock (8.9), all task-only: lockEnabled_ mirrors the setting each
  // tick; lockTarget_ is armed by the first state publish while locked (the
  // lock freezes the *current* state) and re-armed by every queued Freedea
  // edit. Strikes count consecutive overrides that never converged.
  bool lockEnabled_ = false;
  midea::AcState lockTarget_;
  bool lockTargetValid_ = false;
  uint8_t lockStrikes_ = 0;
  uint32_t lockLastRevertMs_ = 0;
  bool resyncEdge_ = false; // mutex-guarded resync request, consumed by the task
  uint8_t pollTimeouts_ = 0;
  bool stateReceived_ = false;
  bool stateChanged_ = false;  // mutex-guarded edge flag, consumed by main
  uint32_t lastStateAtMs_ = 0; // millis of the last received state (publishState)
  bool sessionUp_ = false;     // mutex-guarded mirror of link + session health
  bool reachEdge_ = false;     // mutex-guarded sessionUp_ transition, consumed by main
  // Poll mode (6.1c): pollMode_ is mutex-guarded (main writes via
  // setPollMode, task reads); taskMode_ is task-only bookkeeping.
  PollMode pollMode_ = PollMode::kNone;
  PollMode taskMode_ = PollMode::kNone;
  // Error slot (5.2), mutex-guarded: task writes errorMsg_/errorEdge_, the
  // main loop consumes them into its own buffer for the toast.
  char errorMsg_[40] = {0};
  bool errorEdge_ = false;
};
