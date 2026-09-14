#pragma once

// Fixed-state struct mirroring the parsed fields of a StateResponse
// (msmart command.py StateResponse at d7db53b). std::optional marks fields a
// device omitted (0xFF temperature sentinel or short payload). Trivially
// copyable so the comm task can hand snapshots to the UI under a mutex.

#include <cstdint>
#include <optional>

namespace midea {

// fan_speed value meaning "auto" (command.py:993).
constexpr uint8_t kFanSpeedAuto = 102;

struct AcState {
  bool powerOn = false;
  double targetTemperature = 0.0;
  uint8_t operationalMode = 0;
  uint8_t fanSpeed = 0; // kFanSpeedAuto, else 0..100
  uint8_t swingMode = 0;
  bool turbo = false;
  bool eco = false;
  bool sleep = false;
  bool fahrenheit = false;
  std::optional<double> indoorTemperature;
  std::optional<double> outdoorTemperature;
  bool filterAlert = false;
  bool displayOn = false;
  bool freezeProtection = false; // present only when payload len >= 22
  bool followMe = false;
  bool purifier = false;
  uint8_t errorCode = 0;
  std::optional<uint8_t> targetHumidity; // present only when payload len >= 20
  bool auxHeat = false;
  bool independentAuxHeat = false;
};

} // namespace midea
