#pragma once

// Accumulated parsed fields of the Midea group-data responses (0xC1):
// msmart Group1/2/4/5/7/11Response (command.py:1130-1341 at d7db53b).
// std::optional marks fields never delivered (group unanswered or sentinel,
// e.g. humidity 0). Trivially copyable so the comm task can hand snapshots
// to the UI under a mutex, same contract as AcState.
//
// Group-level coverage is tracked by the caller (a group bitmask plus
// timestamps); this struct carries decoded values only. Fields a group
// answers on every reply (defrost, water pump) are plain bools and mean
// nothing until that group has been seen.

#include <cstdint>
#include <optional>

namespace midea {

struct AcExtStats {
  // Group 1 — outdoor unit performance (Group1Response).
  std::optional<uint8_t> compressorFrequencyHz;
  std::optional<uint8_t> compressorTargetFrequencyHz;
  std::optional<uint8_t> compressorCurrent; // raw; unit undefined upstream (Porti: ~0.1 A)
  std::optional<uint8_t> compressorVoltageV;
  std::optional<double> t1IndoorAmbientC;
  std::optional<double> t2IndoorCoilC;
  std::optional<double> t3OutdoorCoilC;
  std::optional<double> t4OutdoorAmbientC;
  std::optional<uint8_t> dischargePipeC; // TP, raw byte (upstream keeps it raw)

  // Group 2 — indoor fan (Group2Response).
  std::optional<uint16_t> indoorFanRpm;
  std::optional<uint16_t> indoorFanTargetRpm;
  bool waterPumpRunning = false; // valid once group 2 answered

  // Group 4 — energy (Group4Response). Devices encode either BCD or binary;
  // both decodes are kept and the UI decides. All six stay unset (never
  // updated) when every BCD field decodes to zero — midea-msmart's "energy
  // monitoring valid" heuristic.
  std::optional<double> totalEnergyKwhBcd;
  std::optional<double> runEnergyKwhBcd;
  std::optional<double> realTimePowerWBcd;
  std::optional<double> totalEnergyKwhBinary;
  std::optional<double> runEnergyKwhBinary;
  std::optional<double> realTimePowerWBinary;

  // Group 5 — humidity / outdoor fan / defrost (Group5Response).
  // humidityPercent is nullopt while the device reports 0 (no sensor); an
  // answer of 0 clears a previously set value.
  std::optional<uint8_t> humidityPercent;
  std::optional<uint16_t> outdoorFanRpm;
  bool defrost = false; // valid once group 5 answered

  // Group 7 — outdoor unit real-time power draw (Group7Response).
  std::optional<uint16_t> outdoorUnitPowerW;

  // Group 11 — louver angles in degrees (Group11Response).
  std::optional<uint8_t> horizontalLouverDeg;
  std::optional<uint8_t> verticalLouverDeg;
};

} // namespace midea
