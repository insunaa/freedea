// Freedea — AcService task (Phase 4.2). See AcService.h.

#include "AcService.h"

#include <string.h>

#include <Arduino.h>

#include <Commands.h>
#include <Responses.h>
#include <type_traits>

#include "../WifiService.h"

// The capabilities snapshot hands copies of caps_ across the mutex.
static_assert(std::is_trivially_copyable<midea::AcCapabilities>::value,
              "capabilities snapshot relies on trivial copyability of AcCapabilities");
// The 9.2 group-round change gate byte-compares ext_ against extShown_.
static_assert(std::is_trivially_copyable<midea::AcExtStats>::value, "the ext value-edge gate byte-compares AcExtStats");

// acsvc-task scratch for decoded response/push frames. Shared because the
// readers are never reentrant: one task, strictly sequential calls.
static uint8_t sFrameBuf[320];

const devices::Device* AcService::selectTarget(const devices::List& devices) {
  for (uint8_t i = 0; i < devices.count; ++i) {
    const devices::Device& candidate = devices.devices[i];
    const bool knownIp = (candidate.ip[0] | candidate.ip[1] | candidate.ip[2] | candidate.ip[3]) != 0;
    if (knownIp && (candidate.hasCredentials() || candidate.version == 2)) {
      return &candidate;
    }
  }
  return nullptr;
}

bool AcService::init(const devices::List& devices, const WifiService& wifi, const settings::Settings& settings,
                     const midea::AcCapabilities* cachedCaps) {
  const uint32_t heapBefore = ESP.getFreeHeap();
  wifi_ = &wifi;
  settings_ = &settings;

  // First usable record wins; later phases will re-target on demand.
  const devices::Device* target = selectTarget(devices);
  if (target != nullptr) {
    target_ = *target;
    hasTarget_ = true;
  }
  if (hasTarget_) {
    Serial.printf("[AC ] target \"%s\" id=%llu v%u ip=%u.%u.%u.%u:%u token=%u B\n", target_.name,
                  static_cast<unsigned long long>(target_.id), static_cast<unsigned>(target_.version),
                  static_cast<unsigned>(target_.ip[0]), static_cast<unsigned>(target_.ip[1]),
                  static_cast<unsigned>(target_.ip[2]), static_cast<unsigned>(target_.ip[3]),
                  static_cast<unsigned>(target_.port), static_cast<unsigned>(target_.tokenLen));
  } else {
    Serial.println("[AC ] no usable device (need V3 token+key and a known IP); idling");
  }

  // Cache seeding happens before the task starts, so capsKnown_ is race-free.
  if (hasTarget_ && cachedCaps != nullptr) {
    caps_ = *cachedCaps;
    capsReceived_ = true;
    capsKnown_ = true;
    Serial.println("[AC ] capabilities: seeded from SD cache, query will be skipped");
  }

  stateMutex_ = xSemaphoreCreateMutex();
  commandQueue_ = xQueueCreate(kQueueDepth, sizeof(Command));
  const BaseType_t created =
      stateMutex_ != nullptr && commandQueue_ != nullptr &&
      xTaskCreate(&AcService::taskTrampoline, "acsvc", kTaskStackSize, this, kTaskPriority, nullptr) == pdPASS;
  // One-time boot-time allocations: mutex + queue (kQueueDepth * sizeof
  // Command) + the task stack, all taken from the heap at boot while it is
  // still unfragmented.
  Serial.printf("[AC ] init: heap %lu -> %lu\n", static_cast<unsigned long>(heapBefore),
                static_cast<unsigned long>(ESP.getFreeHeap()));
  if (!created) {
    Serial.println("[AC ] init FAILED (mutex/queue/task); service stays inert");
  }
  return created;
}

midea::AcState AcService::snapshot() const {
  // Inert service (init failed or never ran): hand back the default state;
  // its nullopt indoorTemperature is the UI's "no data yet" signal.
  if (stateMutex_ == nullptr) {
    return midea::AcState();
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const midea::AcState copy = state_;
  xSemaphoreGive(stateMutex_);
  return copy;
}

bool AcService::hasState() const {
  if (stateMutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool received = stateReceived_;
  xSemaphoreGive(stateMutex_);
  return received;
}

bool AcService::consumeStateChanged() {
  if (stateMutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool changed = stateChanged_;
  stateChanged_ = false;
  xSemaphoreGive(stateMutex_);
  return changed;
}

bool AcService::sessionUp() const {
  if (stateMutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool up = sessionUp_;
  xSemaphoreGive(stateMutex_);
  return up;
}

uint32_t AcService::lastStateAgeMs() const {
  if (stateMutex_ == nullptr) {
    return 0;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  // Unsigned subtraction: correct across the ~49.7 day millis() wrap.
  const uint32_t ageMs = millis() - lastStateAtMs_;
  xSemaphoreGive(stateMutex_);
  return ageMs;
}

midea::AcExtStats AcService::extSnapshot() const {
  if (stateMutex_ == nullptr) {
    return midea::AcExtStats();
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const midea::AcExtStats copy = ext_;
  xSemaphoreGive(stateMutex_);
  return copy;
}

uint16_t AcService::extGroupsSeen() const {
  if (stateMutex_ == nullptr) {
    return 0;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const uint16_t seen = extSeen_;
  xSemaphoreGive(stateMutex_);
  return seen;
}

std::optional<uint16_t> AcService::outdoorUnitPowerW() const {
  if (stateMutex_ == nullptr) {
    return std::nullopt;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const std::optional<uint16_t> power = ext_.outdoorUnitPowerW;
  xSemaphoreGive(stateMutex_);
  return power;
}

bool AcService::consumeExtUpdate() {
  if (stateMutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool edge = extEdge_;
  extEdge_ = false;
  xSemaphoreGive(stateMutex_);
  return edge;
}

uint32_t AcService::lastExtAgeMs() const {
  if (stateMutex_ == nullptr) {
    return 0;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const uint32_t ageMs = millis() - extAtMs_;
  xSemaphoreGive(stateMutex_);
  return ageMs;
}

void AcService::setPollMode(PollMode mode) {
  if (stateMutex_ == nullptr) {
    pollMode_ = mode; // pre-init: the task applies it when it comes to exist
    return;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  pollMode_ = mode;
  xSemaphoreGive(stateMutex_);
}

bool AcService::consumeReachabilityEdge() {
  if (stateMutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool edge = reachEdge_;
  reachEdge_ = false;
  xSemaphoreGive(stateMutex_);
  return edge;
}

bool AcService::consumeError(char* out, size_t cap) {
  if (stateMutex_ == nullptr) {
    return false;
  }
  bool had = false;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  if (errorEdge_) {
    snprintf(out, cap, "%s", errorMsg_);
    errorEdge_ = false;
    had = true;
  }
  xSemaphoreGive(stateMutex_);
  return had;
}

void AcService::reportError(const char* message) {
  if (stateMutex_ == nullptr) {
    return;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  snprintf(errorMsg_, sizeof(errorMsg_), "%s", message);
  errorEdge_ = true;
  xSemaphoreGive(stateMutex_);
}

midea::AcCapabilities AcService::capabilities() const {
  if (stateMutex_ == nullptr) {
    return midea::AcCapabilities();
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const midea::AcCapabilities copy = caps_;
  xSemaphoreGive(stateMutex_);
  return copy;
}

bool AcService::hasCapabilities() const {
  if (stateMutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool received = capsReceived_;
  xSemaphoreGive(stateMutex_);
  return received;
}

bool AcService::consumeCapabilitiesEdge() {
  if (stateMutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool edge = capsEdge_;
  capsEdge_ = false;
  xSemaphoreGive(stateMutex_);
  return edge;
}

AcService::Props AcService::props() const {
  if (stateMutex_ == nullptr) {
    return Props();
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const Props copy = props_;
  xSemaphoreGive(stateMutex_);
  return copy;
}

bool AcService::consumePropsEdge() {
  if (stateMutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool edge = propsEdge_;
  propsEdge_ = false;
  xSemaphoreGive(stateMutex_);
  return edge;
}

void AcService::notifyLocalChange() {
  if (stateMutex_ == nullptr) {
    return;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  resyncEdge_ = true;
  xSemaphoreGive(stateMutex_);
}

bool AcService::queueCommand(const Command& command) {
  if (commandQueue_ == nullptr) {
    return false;
  }
  // Non-blocking: the caller is the UI task and must never stall on a full
  // queue; dropping the command with a log is the honest failure.
  if (xQueueSend(commandQueue_, &command, 0) != pdTRUE) {
    Serial.println("[AC ] command queue full; command dropped");
    return false;
  }
  // The cached state is stale until the device echoes the change back; ask
  // for the 4.6 fast resync window (the queue send already woke the task).
  notifyLocalChange();
  return true;
}

void AcService::taskTrampoline(void* ctx) {
  static_cast<AcService*>(ctx)->run();
}

void AcService::run() {
  Serial.println("[AC ] task started");
  Command command;
  for (uint32_t ticks = 1;; ++ticks) {
    if (xQueueReceive(commandQueue_, &command, pdMS_TO_TICKS(kTickMs)) == pdTRUE) {
      if (command.kind == Command::Kind::kSetState) {
        // Latest desired state wins: every queued command is a complete desired
        // state (the UI composes from its accumulated edits), so coalescing a
        // burst into the newest one sends exactly what the user last chose.
        pendingDesired_ = command.desired;
        hasPendingSetState_ = true;
        // Every Freedea edit moves the lock target (8.9): the lock follows
        // user intent, not the frozen-at-arm state.
        if (buttonLockEnabled()) {
          lockTarget_ = command.desired;
          lockTargetValid_ = true;
          lockStrikes_ = 0;
        }
        Serial.println("[AC ] setstate queued");
      } else if (command.kind == Command::Kind::kToggleDisplay) {
        // Coalesced to one pending send: two queued toggles cancel out, and
        // one toggle is always what the user last asked for.
        hasPendingToggleDisplay_ = true;
        Serial.println("[AC ] display toggle queued");
      } else {
        // One dirty slot per property kind; a newer value for the same kind
        // supersedes an unsent one, distinct kinds batch into one set. The
        // send is caps-gated (a queued denied property is dropped there).
        const size_t k = static_cast<size_t>(command.propKind);
        if (k < kPropKindCount) {
          pendingPropDirty_[k] = true;
          pendingPropValue_[k] = command.propValue;
          hasPendingProps_ = true;
          Serial.println("[AC ] property set queued");
        }
      }
    }
    serviceSession();
    if (ticks % kHeartbeatsPerLog == 0) {
      // uxTaskGetStackHighWaterMark reports minimum free stack in bytes on
      // ESP-IDF; kTaskStackSize is the ceiling.
      Serial.printf("[AC ] heap=%lu stack_hw=%lu\n", static_cast<unsigned long>(ESP.getFreeHeap()),
                    static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
    }
  }
}

void AcService::serviceSession() {
  if (!hasTarget_ || wifi_ == nullptr) {
    return;
  }
  PollMode mode = takePollMode();
  updateSessionFlags();
  // An armed button lock keeps the session polling even with no AC screen
  // open: detecting an external change needs fresh state (8.9).
  if (updateButtonLockMode() && mode == PollMode::kNone) mode = PollMode::kState;
  if (!wifi_->connected()) {
    if (transport_.alive()) {
      Serial.println("[AC ] wifi link lost; closing session");
      transport_.close();
    }
    return; // WifiService owns reconnection; no backoff noise from here
  }
  // A local change waits for a live session: consume the request only once
  // the radio is up, then poll immediately and hold the fast cadence.
  if (consumeResyncEdge()) {
    Serial.println("[AC ] local change: immediate resync, fast poll window");
    fastPollUntilMs_ = millis() + kFastWindowMs;
    nextPollAtMs_ = millis();
  }
  // A polling mode, a queued control command, or an open fast window need an
  // active session; plain kNone is passive (TCP keepalive + push drains only,
  // no sends and no reconnect attempts — the AC is untouched).
  const bool active = mode != PollMode::kNone || hasPendingSetState_ || hasPendingProps_ || hasPendingToggleDisplay_ ||
                      static_cast<int32_t>(fastPollUntilMs_ - millis()) > 0;
  if (transport_.alive()) {
    if (active && transport_.authExpired()) {
      Serial.println("[AC ] auth expired; reconnecting");
      transport_.close();
    } else {
      // Session parked: consume device pushes first so physical-remote
      // changes land in ~1 s instead of up to one idle poll (silently so in
      // passive mode, where a session the AC dropped is a non-event). A
      // false return means the drain found the session dead.
      if (!drainPushes(!active)) {
        return;
      }
      if (!active) {
        return;
      }
      // A pending control command takes the socket next, so edits are sent
      // without waiting for the next poll slot; otherwise refresh on the
      // adaptive cadence (fast inside the resync window), plus the poll
      // mode's extra groups. Each sender opens the fast window and refreshes
      // the acknowledged truth itself, so a pending command replaces the poll.
      const bool hadSet = hasPendingSetState_;
      const bool hadProps = hasPendingProps_;
      const bool hadToggle = hasPendingToggleDisplay_;
      if (hadSet || hadProps || hadToggle) {
        if (hadSet) {
          sendPendingSetState();
        }
        if (hadProps) {
          sendPendingProps();
        }
        if (hadToggle) {
          sendToggleDisplay();
        }
      } else {
        // Button lock (8.9): a queued override replaces this tick's poll —
        // sendPendingSetState() refreshes the acknowledged truth itself.
        applyButtonLock();
        if (!hasPendingSetState_ && static_cast<int32_t>(millis() - nextPollAtMs_) >= 0) {
          nextPollAtMs_ = millis() + currentPollIntervalMs();
          pullStateOnce();
          pollModeExtras(mode);
        }
      }
      return;
    }
  }
  if (!active) {
    // kNone with a dead session: stay silent. Entering a polling mode clears
    // the backoff and polls immediately, so an AC screen reopens from here.
    return;
  }
  if (static_cast<int32_t>(millis() - retryAtMs_) < 0) {
    return; // inside the backoff window
  }
  if (!transport_.open(target_)) {
    scheduleRetry();
    return;
  }
  backoffMs_ = kFirstBackoffMs;
  // msmart pauses ~1 s after authenticating before the first request;
  // devices respond unreliably right after the handshake.
  vTaskDelay(pdMS_TO_TICKS(1000));
  if (!capsKnown_) {
    fetchCapabilitiesOnce();
    // A fatal caps exchange closes the link and schedules the backoff.
    if (!transport_.alive()) {
      return;
    }
  }
  // Property channel (8.2): once caps are known (this boot or seeded), GET the
  // supported properties. Caps-gated inside, so a device supporting neither
  // sends nothing; a device that never answers leaves the rows hidden.
  fetchPropertiesOnce();
  // A command queued while the link was down goes out before the first poll,
  // then the immediate refresh publishes the acknowledged state.
  const bool hadSet = hasPendingSetState_;
  const bool hadProps = hasPendingProps_;
  const bool hadToggle = hasPendingToggleDisplay_;
  if (hadSet || hadProps || hadToggle) {
    if (hadSet) {
      sendPendingSetState();
    }
    if (hadProps) {
      sendPendingProps();
    }
    if (hadToggle) {
      sendToggleDisplay();
    }
    // No lock check here: the sends' GetState already published the echo.
  } else {
    applyButtonLock();
    if (!hasPendingSetState_) {
      pullStateOnce();
      pollModeExtras(mode);
    }
  }
  nextPollAtMs_ = millis() + currentPollIntervalMs();
}

AcService::PollMode AcService::takePollMode() {
  PollMode requested;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  requested = pollMode_;
  xSemaphoreGive(stateMutex_);
  if (requested == taskMode_) {
    return requested;
  }
  taskMode_ = requested;
  const char* name = requested == PollMode::kNone         ? "none"
                     : requested == PollMode::kState      ? "state"
                     : requested == PollMode::kStatePower ? "state+power"
                                                          : "state+groups";
  Serial.printf("[AC ] poll mode -> %s\n", name);
  if (requested != PollMode::kNone) {
    // Navigate-in: refresh ASAP — skip the idle interval and any reconnect
    // backoff so the screen shows fresh data on arrival.
    nextPollAtMs_ = millis();
    retryAtMs_ = 0;
  }
  return requested;
}

uint32_t AcService::currentPollIntervalMs() {
  // Details mode refreshes faster (3 s) than the generic idle cadence.
  const uint32_t idleMs = taskMode_ == PollMode::kStateAll ? kPollDetailsMs : kPollIdleMs;
  const bool fast = static_cast<int32_t>(fastPollUntilMs_ - millis()) > 0;
  if (fast != fastPollActive_) {
    fastPollActive_ = fast;
    Serial.printf("[AC ] poll cadence: %s %lu s\n", fast ? "fast" : "idle",
                  static_cast<unsigned long>((fast ? kPollFastMs : idleMs) / 1000));
  }
  return fast ? kPollFastMs : idleMs;
}

bool AcService::updateButtonLockMode() {
  const bool enabled = buttonLockEnabled();
  if (enabled != lockEnabled_) {
    lockEnabled_ = enabled;
    // Both transitions re-arm: enabling adopts the *next* published state
    // (the lock freezes whatever the AC is doing now, not some past one);
    // disabling drops the target so a later enable cannot resurrect it.
    lockTargetValid_ = false;
    lockStrikes_ = 0;
    Serial.printf("[AC ] button lock %s\n", enabled ? "on: freezing state, forcing poll" : "off");
  }
  return enabled;
}

void AcService::applyButtonLock() {
  // External-change override (8.9): only with no send in flight (their
  // echoes match the target), an armed target, and a received state.
  if (!lockEnabled_ || !lockTargetValid_ || !stateReceived_ || hasPendingSetState_ || hasPendingProps_ ||
      hasPendingToggleDisplay_) {
    return;
  }
  // Only the fields a SetState carries and unit buttons move — anything else
  // in the frames (temps, display, humidity) is never reverted.
  auto differs = [](const midea::AcState& a, const midea::AcState& b) {
    return a.powerOn != b.powerOn || a.operationalMode != b.operationalMode ||
           a.targetTemperature != b.targetTemperature || a.fanSpeed != b.fanSpeed || a.swingMode != b.swingMode ||
           a.eco != b.eco || a.turbo != b.turbo || a.sleep != b.sleep || a.freezeProtection != b.freezeProtection ||
           a.followMe != b.followMe;
  };
  if (!differs(state_, lockTarget_)) {
    if (lockStrikes_ != 0) {
      lockStrikes_ = 0; // converged again: overrides (and the strike cap) reset
      Serial.println("[AC ] button lock: converged");
    }
    return;
  }
  if (lockStrikes_ >= kLockMaxStrikes) {
    return; // stuck: stopped overriding; reset by convergence, an edit, or a toggle
  }
  if (static_cast<int32_t>(millis() - lockLastRevertMs_) < static_cast<int32_t>(kLockRevertMinMs)) {
    return; // min gap between overrides
  }
  ++lockStrikes_;
  lockLastRevertMs_ = millis();
  pendingDesired_ = lockTarget_;
  hasPendingSetState_ = true; // sendPendingSetState(): send + GetState + fast window
  Serial.printf("[AC ] button lock: overriding external change (attempt %u)\n", static_cast<unsigned>(lockStrikes_));
  reportError(lockStrikes_ >= kLockMaxStrikes ? "Button lock stuck" : "Lock: change reverted");
}

void AcService::updateSessionFlags() {
  const bool up = wifi_->connected() && transport_.alive();
  bool changed = false;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  if (up != sessionUp_) {
    sessionUp_ = up;
    reachEdge_ = true;
    changed = true;
  }
  xSemaphoreGive(stateMutex_);
  if (changed) {
    Serial.printf("[AC ] session %s\n", up ? "up" : "down");
  }
}

bool AcService::consumeResyncEdge() {
  if (stateMutex_ == nullptr) {
    return false;
  }
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  const bool edge = resyncEdge_;
  resyncEdge_ = false;
  xSemaphoreGive(stateMutex_);
  return edge;
}

void AcService::fetchCapabilitiesOnce() {
  // Single-task (acsvc) scratch, static so nothing lands on the 4 KB stack.
  // Frames decode into the shared sFrameBuf.
  static uint8_t sRequest[midea::kCommandMaxFrameLen];

  // One query/parse pass; true when `page` holds decoded records. Frames are
  // read until the capabilities response arrives — a push (or state frame)
  // may precede it — so a state published opportunistically here is a bonus,
  // not the contract. A failed link or protocol error closes the session
  // (the caller reopens with backoff); timeouts leave the session up so the
  // state poll can still run.
  auto query = [&](bool additional, midea::AcCapabilities& page) -> bool {
    const midea::GetCapabilitiesCommand command{additional};
    const size_t frameLen = command.serialize(sRequest, sizeof(sRequest));
    if (frameLen == 0) {
      Serial.println("[AC ] GetCapabilities build failed");
      return false;
    }
    LanTransport::Result result = transport_.sendFrame(sRequest, frameLen);
    bool gotCaps = false;
    size_t capsLen = 0;
    if (result == LanTransport::Result::kOk) {
      const uint32_t deadline = millis() + kResponseTimeoutMs;
      while (!gotCaps) {
        result = transport_.readFrame(sFrameBuf, sizeof(sFrameBuf), &capsLen, deadline);
        if (result != LanTransport::Result::kOk) {
          break;
        }
        const midea::ResponseKind kind =
            capsLen < 13 ? midea::ResponseKind::kInvalid : midea::classifyResponse(sFrameBuf, capsLen);
        if (kind == midea::ResponseKind::kCapabilities) {
          gotCaps = true;
          break;
        }
        if (kind == midea::ResponseKind::kState) {
          midea::AcState state;
          if (midea::parseStateResponse(sFrameBuf + 10, capsLen - 12, state)) {
            publishState(state);
            Serial.println("[AC ] state received (push)");
          }
        }
      }
    }
    if (!gotCaps) {
      Serial.printf("[AC ] caps query failed (additional=%u, result=%u)\n", static_cast<unsigned>(additional),
                    static_cast<unsigned>(result));
      if (result == LanTransport::Result::kLinkDown || result == LanTransport::Result::kProtocolError) {
        transport_.close();
        scheduleRetry();
      }
      return false;
    }
    if (!midea::parseCapabilitiesResponse(sFrameBuf + 10, capsLen - 12, page)) {
      Serial.println("[AC ] CapabilitiesResponse parse failed");
      return false;
    }
    return true;
  };

  // Two fixed-size parse targets instead of merging inside the lambda: the
  // base page's additionalPending flag is consumed explicitly (merge would
  // keep dst's copy, mirroring Python's merge()).
  midea::AcCapabilities caps;
  midea::AcCapabilities page;
  if (!query(false, page)) {
    return;
  }
  const bool additionalPending = page.additionalPending;
  caps = page;
  if (additionalPending && query(true, page)) {
    midea::mergeCapabilities(caps, page);
  }

  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  caps_ = caps;
  capsReceived_ = true;
  capsEdge_ = true;
  xSemaphoreGive(stateMutex_);
  capsKnown_ = true;

  Serial.printf("[AC ] caps: cool=%d heat=%d dry=%d auto=%d eco=%d swV=%d swH=%d ranges=%d (additional=%u)\n",
                static_cast<int>(caps.coolMode), static_cast<int>(caps.heatMode), static_cast<int>(caps.dryMode),
                static_cast<int>(caps.autoMode), static_cast<int>(caps.eco), static_cast<int>(caps.swingVertical),
                static_cast<int>(caps.swingHorizontal), static_cast<int>(caps.hasTemperatureRanges),
                static_cast<unsigned>(additionalPending));
}

void AcService::sendPendingSetState() {
  // Single-task (acsvc) scratch, static so nothing lands on the 4 KB stack.
  static uint8_t sRequest[midea::kCommandMaxFrameLen];

  hasPendingSetState_ = false;
  const midea::AcState& desired = pendingDesired_;
  Serial.printf("[AC ] setstate: power=%u mode=%u temp=%.1f fan=%u swing=0x%02x eco=%u turbo=%u\n",
                static_cast<unsigned>(desired.powerOn), static_cast<unsigned>(desired.operationalMode),
                desired.targetTemperature, static_cast<unsigned>(desired.fanSpeed),
                static_cast<unsigned>(desired.swingMode), static_cast<unsigned>(desired.eco),
                static_cast<unsigned>(desired.turbo));

  midea::SetStateCommand command;
  command.powerOn = desired.powerOn;
  command.targetTemperature = desired.targetTemperature;
  command.operationalMode = desired.operationalMode;
  command.fanSpeed = desired.fanSpeed;
  command.swingMode = desired.swingMode;
  command.eco = desired.eco;
  command.turbo = desired.turbo;
  command.sleep = desired.sleep;
  command.freezeProtection = desired.freezeProtection;
  command.followMe = desired.followMe;
  command.purifier = desired.purifier;
  command.auxHeat = desired.auxHeat;
  command.independentAuxHeat = desired.independentAuxHeat;
  // The AC chirps on commands iff the Sound setting is on (8.8).
  command.beepOn = beepEnabled();
  if (desired.targetHumidity.has_value()) {
    command.targetHumidity = *desired.targetHumidity;
  }
  // Freedea's UI and state model are Celsius; the wire unit flag follows.
  command.fahrenheit = false;

  const size_t frameLen = command.serialize(sRequest, sizeof(sRequest));
  if (frameLen == 0) {
    Serial.println("[AC ] setstate build failed");
  } else {
    LanTransport::Result result = transport_.sendFrame(sRequest, frameLen);
    if (result == LanTransport::Result::kOk) {
      // The device answers a SetState with a state frame carrying the
      // applied values; consume it (and any push queued ahead of it).
      result = readStateUntil(millis() + kResponseTimeoutMs, " (ack)");
    }
    if (result != LanTransport::Result::kOk) {
      // Whatever failed, the resync below reports the device's truth: on a
      // dead link pullStateOnce() closes and schedules the backoff; on a
      // timeout it counts toward the session-drop threshold.
      Serial.printf("[AC ] setstate send failed (result=%u)\n", static_cast<unsigned>(result));
      reportError(result == LanTransport::Result::kTimeout ? "AC command not acknowledged" : "AC command failed");
    }
  }
  // Immediate GetState: the acknowledged state lands in the snapshot now
  // rather than up to a poll interval later, and the fast window catches any
  // further echo (e.g. the device rejecting part of the command).
  pullStateOnce();
  fastPollUntilMs_ = millis() + kFastWindowMs;
  nextPollAtMs_ = millis() + currentPollIntervalMs();
}

void AcService::sendToggleDisplay() {
  // Single-task (acsvc) scratch.
  static uint8_t sRequest[midea::kCommandMaxFrameLen];

  hasPendingToggleDisplay_ = false;

  // QUERY-frame command with no explicit set (command.py:382-407): it flips
  // the display and answers like a state query at best, so no dedicated ack
  // read here — the GetState below reports the new displayOn either way (a
  // device echo landing first is consumed as a free refresh by its read loop).
  midea::ToggleDisplayCommand command;
  command.beepOn = beepEnabled(); // Sound setting (8.8)
  Serial.printf("[AC ] display toggle: beep=%u\n", static_cast<unsigned>(command.beepOn));
  const size_t frameLen = command.serialize(sRequest, sizeof(sRequest));
  if (frameLen == 0) {
    Serial.println("[AC ] display toggle build failed");
  } else {
    const LanTransport::Result result = transport_.sendFrame(sRequest, frameLen);
    if (result != LanTransport::Result::kOk) {
      Serial.printf("[AC ] display toggle send failed (result=%u)\n", static_cast<unsigned>(result));
      reportError("AC display toggle failed");
    }
  }
  pullStateOnce();
  fastPollUntilMs_ = millis() + kFastWindowMs;
  nextPollAtMs_ = millis() + currentPollIntervalMs();
}

LanTransport::Result AcService::readUntilPropertyResponse(midea::ResponseKind want, uint32_t deadlineMs,
                                                          size_t* outLen) {
  // Same tolerant read loop as readStateUntil: interleaved state frames are a
  // free refresh, anything else is skipped and logged. Returns on the wanted
  // property frame, a link error, or the deadline.
  for (;;) {
    size_t frameLen = 0;
    const LanTransport::Result r = transport_.readFrame(sFrameBuf, sizeof(sFrameBuf), &frameLen, deadlineMs);
    if (r != LanTransport::Result::kOk) {
      return r;
    }
    const midea::ResponseKind kind =
        frameLen < 13 ? midea::ResponseKind::kInvalid : midea::classifyResponse(sFrameBuf, frameLen);
    if (kind == want) {
      *outLen = frameLen;
      return LanTransport::Result::kOk;
    }
    if (kind == midea::ResponseKind::kState) {
      midea::AcState state;
      if (midea::parseStateResponse(sFrameBuf + 10, frameLen - 12, state)) {
        publishState(state);
      }
      continue;
    }
    Serial.printf("[AC ] prop read: skipping frame (kind=%u id=0x%02x)\n", static_cast<unsigned>(kind),
                  frameLen >= 13 ? static_cast<unsigned>(sFrameBuf[10]) : 0U);
  }
}

void AcService::applyProperties(const midea::AcPropertyValues& values) {
  bool changed = false;
  // Merge one decoded property into the snapshot; a failed TLV still stores its
  // value (mirrors upstream: the failure bit is diagnostic, not absence) and
  // known flips on the first answer.
  const auto merge = [&changed](bool has, bool value, bool& slot, bool& known) {
    if (!has) return;
    if (!known || slot != value) changed = true;
    slot = value;
    known = true;
  };
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  merge(values.hasIeco, values.iecoOn, props_.ieco, props_.iecoKnown);
  merge(values.hasOutSilent, values.outSilentOn, props_.outSilent, props_.outSilentKnown);
  merge(values.hasSelfClean, values.selfCleanOn, props_.selfCleanRunning, props_.selfCleanKnown);
  merge(values.hasBreezeAway, values.breezeAwayOn, props_.breezeAway, props_.breezeAwayKnown);
  merge(values.hasBreezeless, values.breezelessOn, props_.breezeless, props_.breezelessKnown);
  merge(values.hasFlash, values.flashOn, props_.flash, props_.flashKnown);
  // 8.6 enum features merge the raw value byte instead of a bool.
  const auto mergeByte = [&changed](bool has, uint8_t value, uint8_t& slot, bool& known) {
    if (!has) return;
    if (!known || slot != value) changed = true;
    slot = value;
    known = true;
  };
  mergeByte(values.hasRateSelect, values.rateSelect, props_.rateSelect, props_.rateSelectKnown);
  mergeByte(values.hasCascade, values.cascade, props_.cascade, props_.cascadeKnown);
  mergeByte(values.hasFreshAir, values.freshAir, props_.freshAir, props_.freshAirKnown);
  if (changed) {
    propsEdge_ = true;
  }
  xSemaphoreGive(stateMutex_);

  // Logging outside the mutex; the failed bits mirror upstream (value stored,
  // failure reported) so a device that answers with the fail bit is visible.
  struct LogEntry {
    bool has;
    bool on;
    bool failed;
    const char* name;
  };
  const LogEntry entries[] = {
      {values.hasIeco, values.iecoOn, values.iecoFailed, "ieco"},
      {values.hasOutSilent, values.outSilentOn, values.outSilentFailed, "out_silent"},
      {values.hasSelfClean, values.selfCleanOn, values.selfCleanFailed, "self_clean"},
      {values.hasBreezeAway, values.breezeAwayOn, values.breezeAwayFailed, "breeze_away"},
      {values.hasBreezeless, values.breezelessOn, values.breezelessFailed, "breezeless"},
      {values.hasFlash, values.flashOn, values.flashFailed, "flash"},
  };
  for (const LogEntry& entry : entries) {
    if (entry.has) {
      Serial.printf("[AC ] prop: %s=%u%s\n", entry.name, static_cast<unsigned>(entry.on),
                    entry.failed ? " (failed)" : "");
    }
  }
  struct ByteLogEntry {
    bool has;
    uint8_t value;
    bool failed;
    const char* name;
  };
  const ByteLogEntry byteEntries[] = {
      {values.hasRateSelect, values.rateSelect, values.rateSelectFailed, "rate_select"},
      {values.hasCascade, values.cascade, values.cascadeFailed, "cascade"},
      {values.hasFreshAir, values.freshAir, values.freshAirFailed, "fresh_air"},
  };
  for (const ByteLogEntry& entry : byteEntries) {
    if (entry.has) {
      Serial.printf("[AC ] prop: %s=%u%s\n", entry.name, static_cast<unsigned>(entry.value),
                    entry.failed ? " (failed)" : "");
    }
  }
}

void AcService::fetchPropertiesOnce() {
  // Single-task (acsvc) scratch; responses land in the shared sFrameBuf.
  static uint8_t sRequest[midea::kCommandMaxFrameLen];

  const midea::AcCapabilities caps = capabilities();
  midea::GetPropertiesCommand command;
  command.queryIeco = caps.ieco == midea::CapFlag::kTrue;
  command.queryOutSilent = caps.outSilent == midea::CapFlag::kTrue;
  command.querySelfClean = caps.selfClean == midea::CapFlag::kTrue;
  command.queryBreezeAway = caps.breezeAway == midea::CapFlag::kTrue;
  command.queryBreezeless = caps.breezeless == midea::CapFlag::kTrue;
  command.queryFlash = caps.flash == midea::CapFlag::kTrue;
  command.queryRateSelect =
      caps.rateSelect2Level == midea::CapFlag::kTrue || caps.rateSelect5Level == midea::CapFlag::kTrue;
  command.queryCascade = caps.cascade == midea::CapFlag::kTrue;
  command.queryFreshAir = caps.freshAir == midea::CapFlag::kTrue;
  if (!command.queryIeco && !command.queryOutSilent && !command.querySelfClean && !command.queryBreezeAway &&
      !command.queryBreezeless && !command.queryFlash && !command.queryRateSelect && !command.queryCascade &&
      !command.queryFreshAir) {
    return; // caps support none of them; querying an unsupported property errors
  }

  const size_t frameLen = command.serialize(sRequest, sizeof(sRequest));
  if (frameLen == 0) {
    Serial.println("[AC ] prop get build failed");
    return;
  }
  if (transport_.sendFrame(sRequest, frameLen) != LanTransport::Result::kOk) {
    Serial.println("[AC ] prop get send failed");
    return; // link trouble; surfaced by the next state poll
  }
  size_t len = 0;
  const LanTransport::Result r =
      readUntilPropertyResponse(midea::ResponseKind::kProperties, millis() + kResponseTimeoutMs, &len);
  if (r != LanTransport::Result::kOk) {
    Serial.printf("[AC ] prop get no response (result=%u)\n", static_cast<unsigned>(r));
    return;
  }
  midea::AcPropertyValues values;
  if (!midea::parsePropertyResponse(sFrameBuf + 10, len - 12, values)) {
    Serial.println("[AC ] prop response parse failed");
    return;
  }
  applyProperties(values);
}

void AcService::sendPendingProps() {
  // Single-task (acsvc) scratch; responses land in the shared sFrameBuf.
  static uint8_t sRequest[midea::kCommandMaxFrameLen];

  // Snapshot + clear the dirty set (task-only): a set failure still clears,
  // matching setState (the re-query reports the device's truth either way).
  bool dirty[kPropKindCount];
  uint8_t values[kPropKindCount];
  for (size_t k = 0; k < kPropKindCount; ++k) {
    dirty[k] = pendingPropDirty_[k];
    values[k] = pendingPropValue_[k];
    pendingPropDirty_[k] = false;
  }
  hasPendingProps_ = false;

  const auto dirtyValue = [&dirty, &values](PropKind kind) {
    const size_t k = static_cast<size_t>(kind);
    return dirty[k] ? values[k] : 0;
  };
  const auto isDirty = [&dirty](PropKind kind) { return dirty[static_cast<size_t>(kind)]; };

  const midea::AcCapabilities caps = capabilities();
  midea::SetPropertiesCommand command;
  // Caps-gate: a queued property the device does not report is dropped (the UI
  // only offers caps-true rows, so this only guards against a race).
  if (isDirty(PropKind::kIeco) && caps.ieco == midea::CapFlag::kTrue) {
    command.setIeco = true;
    command.iecoNumber = caps.iecoNumber; // echo the caps-derived level count
    command.iecoOn = dirtyValue(PropKind::kIeco) != 0;
  }
  if (isDirty(PropKind::kOutSilent) && caps.outSilent == midea::CapFlag::kTrue) {
    command.setOutSilent = true;
    command.outSilentOn = dirtyValue(PropKind::kOutSilent) != 0;
  }
  if (isDirty(PropKind::kSelfClean) && caps.selfClean == midea::CapFlag::kTrue) {
    command.setSelfClean = true; // one-shot trigger; the value byte is unused
  }
  if (isDirty(PropKind::kBreezeAway) && caps.breezeAway == midea::CapFlag::kTrue) {
    command.setBreezeAway = true;
    command.breezeAwayOn = dirtyValue(PropKind::kBreezeAway) != 0;
  }
  if (isDirty(PropKind::kBreezeless) && caps.breezeless == midea::CapFlag::kTrue) {
    command.setBreezeless = true;
    command.breezelessOn = dirtyValue(PropKind::kBreezeless) != 0;
  }
  if (isDirty(PropKind::kFlash) && caps.flash == midea::CapFlag::kTrue) {
    command.setFlash = true;
    command.flashOn = dirtyValue(PropKind::kFlash) != 0;
  }
  if (isDirty(PropKind::kRateSelect) &&
      (caps.rateSelect2Level == midea::CapFlag::kTrue || caps.rateSelect5Level == midea::CapFlag::kTrue)) {
    command.setRateSelect = true;
    command.rateSelectValue = dirtyValue(PropKind::kRateSelect);
  }
  if (isDirty(PropKind::kCascade) && caps.cascade == midea::CapFlag::kTrue) {
    command.setCascade = true;
    command.cascadeMode = dirtyValue(PropKind::kCascade);
  }
  if (isDirty(PropKind::kFreshAir) && caps.freshAir == midea::CapFlag::kTrue) {
    command.setFreshAir = true;
    command.freshAirSpeed = dirtyValue(PropKind::kFreshAir);
  }
  // BUZZER (8.8): deliberately not caps-gated — the msmart caps reader for
  // 0x022C gates nothing, and msmart sends the buzzer write to every device.
  if (isDirty(PropKind::kBeep)) {
    command.setBuzzer = true;
    command.buzzerOn = dirtyValue(PropKind::kBeep) != 0;
  }
  if (!command.setIeco && !command.setOutSilent && !command.setSelfClean && !command.setBreezeAway &&
      !command.setBreezeless && !command.setFlash && !command.setRateSelect && !command.setCascade &&
      !command.setFreshAir && !command.setBuzzer) {
    return; // nothing supported to send
  }

  // One log line per property actually included in the frame.
  struct SentEntry {
    bool included;
    const char* name;
    uint8_t value;
  };
  const SentEntry sent[] = {
      {command.setIeco, "ieco", command.iecoOn ? 1U : 0U},
      {command.setOutSilent, "out_silent", command.outSilentOn ? 1U : 0U},
      {command.setSelfClean, "self_clean", 1U},
      {command.setBreezeAway, "breeze_away", command.breezeAwayOn ? 1U : 0U},
      {command.setBreezeless, "breezeless", command.breezelessOn ? 1U : 0U},
      {command.setFlash, "flash", command.flashOn ? 1U : 0U},
      {command.setRateSelect, "rate_select", command.rateSelectValue},
      {command.setCascade, "cascade", command.cascadeMode},
      {command.setFreshAir, "fresh_air", command.freshAirSpeed},
      {command.setBuzzer, "buzzer", command.buzzerOn ? 1U : 0U},
  };
  for (const SentEntry& entry : sent) {
    if (entry.included) {
      Serial.printf("[AC ] prop set: %s=%u\n", entry.name, static_cast<unsigned>(entry.value));
    }
  }

  const size_t frameLen = command.serialize(sRequest, sizeof(sRequest));
  if (frameLen == 0) {
    Serial.println("[AC ] prop set build failed");
  } else if (transport_.sendFrame(sRequest, frameLen) == LanTransport::Result::kOk) {
    size_t len = 0;
    const LanTransport::Result r =
        readUntilPropertyResponse(midea::ResponseKind::kPropertiesAck, millis() + kResponseTimeoutMs, &len);
    if (r == LanTransport::Result::kOk) {
      midea::AcPropertyAck ack;
      if (midea::parsePropertyAck(sFrameBuf + 10, len - 12, ack)) {
        struct AckEntry {
          bool saw;
          bool failed;
          const char* name;
        };
        const AckEntry entries[] = {
            {ack.sawIeco, ack.iecoFailed, "ieco"},
            {ack.sawOutSilent, ack.outSilentFailed, "out_silent"},
            {ack.sawSelfClean, ack.selfCleanFailed, "self_clean"},
            {ack.sawBreezeAway, ack.breezeAwayFailed, "breeze_away"},
            {ack.sawBreezeless, ack.breezelessFailed, "breezeless"},
            {ack.sawFlash, ack.flashFailed, "flash"},
            {ack.sawRateSelect, ack.rateSelectFailed, "rate_select"},
            {ack.sawCascade, ack.cascadeFailed, "cascade"},
            {ack.sawFreshAir, ack.freshAirFailed, "fresh_air"},
            {ack.sawBuzzer, ack.buzzerFailed, "buzzer"},
        };
        for (const AckEntry& entry : entries) {
          if (entry.saw) {
            Serial.printf("[AC ] prop: %s ack %s\n", entry.name, entry.failed ? "FAILED" : "ok");
          }
        }
      }
    } else {
      Serial.printf("[AC ] prop set: no ack (result=%u)\n", static_cast<unsigned>(r));
    }
  } else {
    Serial.println("[AC ] prop set send failed");
  }

  // Acknowledged state only: re-query for display rather than trusting the
  // ack's value, and open the fast window like setState.
  fetchPropertiesOnce();
  fastPollUntilMs_ = millis() + kFastWindowMs;
  nextPollAtMs_ = millis() + currentPollIntervalMs();
}

void AcService::scheduleRetry() {
  Serial.printf("[AC ] retrying connection in %lu s\n", static_cast<unsigned long>(backoffMs_ / 1000));
  retryAtMs_ = millis() + backoffMs_;
  if (backoffMs_ < kMaxBackoffMs) {
    backoffMs_ *= 2;
  }
}

void AcService::pullStateOnce() {
  // Single-task (acsvc) scratch, static so nothing lands on the 4 KB stack.
  static uint8_t sRequest[midea::kCommandMaxFrameLen];

  const size_t frameLen = midea::GetStateCommand().serialize(sRequest, sizeof(sRequest));
  if (frameLen == 0) {
    Serial.println("[AC ] GetState build failed");
    return;
  }
  LanTransport::Result result = transport_.sendFrame(sRequest, frameLen);
  if (result == LanTransport::Result::kOk) {
    result = readStateUntil(millis() + kResponseTimeoutMs, "");
  }
  if (result == LanTransport::Result::kOk) {
    pollTimeouts_ = 0;
    return;
  }
  Serial.printf("[AC ] getstate failed (result=%u)\n", static_cast<unsigned>(result));
  if (result == LanTransport::Result::kProtocolError) {
    reportError("AC protocol error - reconnecting");
    transport_.close();
    pollTimeouts_ = 0;
    scheduleRetry();
  } else if (result == LanTransport::Result::kLinkDown) {
    reportError("AC link lost - reconnecting");
    transport_.close();
    pollTimeouts_ = 0;
    scheduleRetry();
  } else if (++pollTimeouts_ >= kMaxPollTimeouts) {
    // Silent device on a live socket: drop it so the backoff reconnect
    // re-authenticates instead of polling a dead session forever.
    Serial.println("[AC ] poll timeouts exceeded; dropping session");
    reportError("AC not responding - retrying");
    pollTimeouts_ = 0;
    transport_.close();
    scheduleRetry();
  }
}

LanTransport::Result AcService::readStateUntil(uint32_t deadlineMs, const char* logSuffix) {
  for (;;) {
    size_t frameLen = 0;
    const LanTransport::Result r = transport_.readFrame(sFrameBuf, sizeof(sFrameBuf), &frameLen, deadlineMs);
    if (r != LanTransport::Result::kOk) {
      return r;
    }
    const midea::ResponseKind kind =
        frameLen < 13 ? midea::ResponseKind::kInvalid : midea::classifyResponse(sFrameBuf, frameLen);
    if (kind == midea::ResponseKind::kState) {
      midea::AcState state;
      if (!midea::parseStateResponse(sFrameBuf + 10, frameLen - 12, state)) {
        Serial.println("[AC ] StateResponse parse failed");
        reportError("AC bad state frame ignored");
        continue;
      }
      publishState(state);
      if (state.indoorTemperature.has_value()) {
        Serial.printf("[AC ] state received%s: power=%u indoor=%.1f C\n", logSuffix,
                      static_cast<unsigned>(state.powerOn), *state.indoorTemperature);
      } else {
        Serial.printf("[AC ] state received%s: power=%u indoor=n/a\n", logSuffix, static_cast<unsigned>(state.powerOn));
      }
      return LanTransport::Result::kOk;
    }
    // Not a state frame: a device push or property traffic we do not parse.
    // The response id (frame[10]) makes captures diagnosable. kInvalid means
    // a frame that failed framing/checksum: log + toast, session survives
    // (the stream stays aligned; only whole packets are consumed).
    if (kind == midea::ResponseKind::kInvalid) {
      reportError("AC malformed frame ignored");
    }
    Serial.printf("[AC ] skipping frame (kind=%u id=0x%02x)\n", static_cast<unsigned>(kind),
                  frameLen >= 13 ? static_cast<unsigned>(sFrameBuf[10]) : 0U);
  }
}

bool AcService::drainPushes(bool silent) {
  // Bound the burst: pushes arrive in short clusters and each buffered frame
  // reads without waiting, so 4 iterations keep the tick ~1 s as designed;
  // anything further is picked up next tick (or by the next poll).
  for (uint8_t i = 0; i < 4; ++i) {
    if (!transport_.hasReadableData()) {
      return true;
    }
    size_t frameLen = 0;
    const LanTransport::Result r = transport_.readFrame(sFrameBuf, sizeof(sFrameBuf), &frameLen, millis() + 50);
    if (r == LanTransport::Result::kTimeout) {
      return true; // partial frame; the buffer resumes it next tick
    }
    if (r != LanTransport::Result::kOk) {
      Serial.println(silent ? "[AC ] passive drain found dead session" : "[AC ] push read failed; dropping session");
      transport_.close();
      if (silent) {
        // kNone mode: an idle-dropped session is the expected end of an idle
        // session, not an error — no toast, no backoff; entering a polling
        // mode reopens immediately.
        return false;
      }
      reportError(r == LanTransport::Result::kProtocolError ? "AC protocol error - reconnecting"
                                                            : "AC link lost - reconnecting");
      scheduleRetry();
      return false;
    }
    const midea::ResponseKind kind =
        frameLen < 13 ? midea::ResponseKind::kInvalid : midea::classifyResponse(sFrameBuf, frameLen);
    if (kind != midea::ResponseKind::kState) {
      Serial.printf("[AC ] push skipped (kind=%u id=0x%02x)\n", static_cast<unsigned>(kind),
                    frameLen >= 13 ? static_cast<unsigned>(sFrameBuf[10]) : 0U);
      continue;
    }
    midea::AcState state;
    if (!midea::parseStateResponse(sFrameBuf + 10, frameLen - 12, state)) {
      Serial.println("[AC ] pushed StateResponse parse failed");
      reportError("AC bad state frame ignored");
      continue;
    }
    publishState(state);
    if (state.indoorTemperature.has_value()) {
      Serial.printf("[AC ] state received (push): power=%u indoor=%.1f C\n", static_cast<unsigned>(state.powerOn),
                    *state.indoorTemperature);
    } else {
      Serial.printf("[AC ] state received (push): power=%u indoor=n/a\n", static_cast<unsigned>(state.powerOn));
    }
  }
  return true;
}

void AcService::pollModeExtras(PollMode mode) {
  static constexpr uint8_t kPowerGroup = 7;
  static constexpr uint8_t kAllGroups[] = {1, 2, 4, 5, 7, 11};
  if (!transport_.alive()) {
    return; // the state poll's failure path already reported and closed
  }
  if (mode == PollMode::kStatePower) {
    pollGroups(&kPowerGroup, 1);
  } else if (mode == PollMode::kStateAll) {
    pollGroups(kAllGroups, sizeof(kAllGroups));
  }
}

void AcService::pollGroups(const uint8_t* groups, size_t count) {
  // Single-task (acsvc) scratch; responses land in the shared sFrameBuf.
  static uint8_t sRequest[midea::kCommandMaxFrameLen];
  size_t answered = 0;
  bool published = false;
  bool linkError = false;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t group = groups[i];
    const midea::GetGroupDataCommand command{group};
    const size_t frameLen = command.serialize(sRequest, sizeof(sRequest));
    if (frameLen == 0) {
      continue;
    }
    if (transport_.sendFrame(sRequest, frameLen) != LanTransport::Result::kOk) {
      Serial.printf("[AC ] g%u send failed\n", group);
      break; // link trouble; the next tick's drain/poll surfaces it properly
    }
    const uint32_t deadline = millis() + kGroupResponseMs;
    for (;;) {
      size_t rlen = 0;
      const LanTransport::Result r = transport_.readFrame(sFrameBuf, sizeof(sFrameBuf), &rlen, deadline);
      if (r == LanTransport::Result::kTimeout) {
        break; // this group stays silent for now; next group's turn
      }
      if (r != LanTransport::Result::kOk) {
        linkError = true;
        break; // link error mid-round; handled by the next tick
      }
      uint8_t got = 0;
      const midea::ResponseKind kind =
          rlen < 13 ? midea::ResponseKind::kInvalid : midea::classifyResponse(sFrameBuf, rlen, &got);
      if (kind == midea::ResponseKind::kState) {
        // Interleaved state/push frame: a free refresh, keep the round going.
        midea::AcState state;
        if (midea::parseStateResponse(sFrameBuf + 10, rlen - 12, state)) {
          publishState(state);
        }
        continue;
      }
      if (kind != midea::ResponseKind::kGroupData) {
        continue;
      }
      if (publishExt(got, sFrameBuf + 10, rlen - 12)) {
        ++answered;
        published = true;
      }
      if (got == group) {
        break; // the requested answer landed; cross-talk already recorded
      }
    }
    if (linkError) {
      break;
    }
  }
  // Repaint edge per completed round, not per group — and only when the
  // round actually moved a number: the parsed group fields are byte-compared
  // against extShown_ (the values last announced), so a round of unchanged
  // values never reaches the panel. The byte compare is sound because
  // parseGroupData only ever assigns fields: std::optional padding bytes
  // never move, so equal bytes ⇔ equal values once extShown_ is seeded by
  // memcpy.
  if (published) {
    xSemaphoreTake(stateMutex_, portMAX_DELAY);
    if (memcmp(&ext_, &extShown_, sizeof(ext_)) != 0) {
      extEdge_ = true;
      memcpy(&extShown_, &ext_, sizeof(ext_));
    }
    xSemaphoreGive(stateMutex_);
  }
  Serial.printf("[AC ] group round %u/%u answered seen=0x%04x\n", static_cast<unsigned>(answered),
                static_cast<unsigned>(count), extSeen_);
}

bool AcService::publishExt(uint8_t group, const uint8_t* payload, size_t len) {
  bool parsed = false;
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  if (midea::parseGroupData(group, payload, len, ext_)) {
    extSeen_ |= static_cast<uint16_t>(1u << group);
    extAtMs_ = millis();
    // extEdge_ is raised by pollGroups once the whole round is done and its
    // values differ from the last announced snapshot.
    parsed = true;
  }
  xSemaphoreGive(stateMutex_);
  return parsed;
}

void AcService::publishState(const midea::AcState& state) {
  xSemaphoreTake(stateMutex_, portMAX_DELAY);
  // E-ink discipline: raise the repaint edge only when a field the UI shows
  // changed — the Dashboard's power/indoor/target/mode/fan/swing plus the
  // Control screen's eco/turbo toggles and the display row (8.7).
  if (!stateReceived_ || state.powerOn != state_.powerOn || state.indoorTemperature != state_.indoorTemperature ||
      state.targetTemperature != state_.targetTemperature || state.operationalMode != state_.operationalMode ||
      state.fanSpeed != state_.fanSpeed || state.swingMode != state_.swingMode || state.eco != state_.eco ||
      state.turbo != state_.turbo || state.displayOn != state_.displayOn) {
    stateChanged_ = true;
  }
  state_ = state;
  stateReceived_ = true;
  lastStateAtMs_ = millis();
  // A state just arrived, so the session is by definition up; set here too
  // (not only in updateSessionFlags) so the first pull after a reconnect
  // never renders the stale indicator next to a freshly published state.
  if (!sessionUp_) {
    sessionUp_ = true;
    reachEdge_ = true;
  }
  xSemaphoreGive(stateMutex_);
  // Button lock (8.9): the first state seen while locked arms the target —
  // the lock freezes the state current at lock-on, not some past one. Task-
  // only members, so this runs outside the mutex on the task's own copy.
  if (lockEnabled_ && !lockTargetValid_) {
    lockTarget_ = state;
    lockTargetValid_ = true;
    Serial.println("[AC ] button lock armed");
  }
}
