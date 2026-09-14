#pragma once

// Midea AC command builders: shared message-id counter, GetState, SetState,
// GetCapabilities, ToggleDisplay, Get/SetProperties.
// Port of msmart command.py:185-455 at d7db53b.

#include <cstddef>
#include <cstdint>

#include "Frame.h"

namespace midea {

// Command.CONTROL_SOURCE (command.py:188): "App control".
constexpr uint8_t kControlSource = 0x02;

// TemperatureType (command.py:179-183).
constexpr uint8_t kTemperatureTypeIndoor = 0x02;
constexpr uint8_t kTemperatureTypeOutdoor = 0x03;

// Command._message_id (command.py:190) is one class-level counter shared by
// all commands, pre-incremented and masked to a byte, so the first id sent is
// 0x01. Non-atomic by design: the comm task is the single writer.
// resetMessageId() exists for host-test determinism only.
uint8_t nextMessageId();
void resetMessageId(uint8_t value);

constexpr size_t kGetStateDataLen = 21;
constexpr size_t kSetStateDataLen = 24;
constexpr size_t kToggleDisplayDataLen = 21;
constexpr size_t kGetGroupDataDataLen = 20;

// Worst-case SetPropertiesCommand data: header + iECO TLV + six 1-byte
// TLVs (out-silent, self-clean, breeze-away, breezeless, flash, buzzer) +
// rate-select (1-byte value), cascade (2-byte TLV) and fresh-air (3-byte TLV).
constexpr size_t kPropertiesMaxDataLen = 2 + (3 + 13) + 6 * (3 + 1) + (3 + 1) + (3 + 2) + (3 + 3);

// Largest frame here: the widest command data + message id + CRC8, wrapped
// in a frame.
constexpr size_t kMaxCommandData = kPropertiesMaxDataLen > kSetStateDataLen ? kPropertiesMaxDataLen : kSetStateDataLen;
constexpr size_t kCommandMaxFrameLen = kFrameHeaderLen + kMaxCommandData + 2 + 1;

// GetStateCommand (command.py:226-248): QUERY, 21-byte data.
struct GetStateCommand {
  uint8_t temperatureType = kTemperatureTypeIndoor;

  // Builds the full frame (header + data + message id + CRC8 + checksum),
  // consuming one message id. Returns the frame length, or 0 if outCap is too
  // small.
  size_t serialize(uint8_t* out, size_t outCap) const;
};

// SetStateCommand (command.py:270-379): CONTROL, 24-byte data. Field defaults
// mirror the Python __init__ (273-291).
struct SetStateCommand {
  bool beepOn = true;
  bool powerOn = false;
  double targetTemperature = 25.0;
  uint8_t operationalMode = 0;
  uint8_t fanSpeed = 0;
  bool eco = true;
  uint8_t swingMode = 0;
  bool turbo = false;
  bool fahrenheit = true;
  bool sleep = false;
  bool freezeProtection = false;
  bool followMe = false;
  bool purifier = false;
  uint8_t targetHumidity = 40;
  bool auxHeat = false;
  bool forceAuxHeat = false;
  bool independentAuxHeat = false;

  size_t serialize(uint8_t* out, size_t outCap) const;
};

// GetCapabilitiesCommand (command.py:208-224): QUERY; 3-byte data, or a
// 4-byte request for the additional capability page.
struct GetCapabilitiesCommand {
  bool additional = false;

  size_t serialize(uint8_t* out, size_t outCap) const;
};

// ToggleDisplayCommand (command.py:382-407): QUERY frame type despite being
// a control-like command, 21-byte data.
struct ToggleDisplayCommand {
  bool beepOn = true;

  size_t serialize(uint8_t* out, size_t outCap) const;
};

// GetGroupDataCommand (command.py:251-268): QUERY, 20-byte data. The group
// number rides in the low nibble of byte 3 over a 0x40 base; responses come
// back as 0xC1 with the group at payload byte 3 (frame[13] & 0x0F).
struct GetGroupDataCommand {
  uint8_t group = 0;

  size_t serialize(uint8_t* out, size_t outCap) const;
};

// GetPropertiesCommand (command.py:409-426): QUERY; data = 0xB1, count,
// [id LE16] per queried property. Answers arrive as 0xB1 frames.
// At least one query flag must be set; serialize returns 0 otherwise.
struct GetPropertiesCommand {
  bool queryIeco = false;       // PropertyId.IECO (0x00E3)
  bool queryOutSilent = false;  // PropertyId.OUT_SILENT (0x00CD)
  bool querySelfClean = false;  // PropertyId.SELF_CLEAN (0x0039)
  bool queryBreezeAway = false; // PropertyId.BREEZE_AWAY (0x0042)
  bool queryBreezeless = false; // PropertyId.BREEZELESS (0x0018)
  bool queryFlash = false;      // PropertyId.FLASH (0x0067)
  bool queryRateSelect = false; // PropertyId.RATE_SELECT (0x0048)
  bool queryCascade = false;    // PropertyId.CASCADE (0x0059)
  bool queryFreshAir = false;   // PropertyId.FRESH_AIR (0x004B)

  size_t serialize(uint8_t* out, size_t outCap) const;
};

// SetPropertiesCommand (command.py:429-455): CONTROL; data = 0xB0, count,
// [id LE16, len, value] per set property. Values mirror PropertyId.encode:
// iECO 13 bytes [0, ieco_number, on?1:0, zeros], where ieco_number is the
// caps-derived level count (1/3/8); out-silent 1 byte [on?3:0]; self-clean
// 1 byte [1], a one-shot trigger (msmart only ever sends True); breeze-away
// 1 byte [on?2:1]; breezeless/flash 1 byte [on?1:0]; rate select 1 byte raw
// gear (100 off); cascade 2 bytes [mode != 0 ? 1 : 0, mode]; fresh air 3 bytes
// [speed != 0 ? 1 : 0, speed, 0xFF]. Buzzer is 1 byte [on ? 1 : 0] and always
// the last TLV, mirroring msmart's device layer appending BUZZER after the
// caller's properties (device.py:779-780). Acks arrive as 0xB0 frames. At
// least one set flag must be given; serialize returns 0 otherwise.
struct SetPropertiesCommand {
  bool setIeco = false;
  uint8_t iecoNumber = 1;
  bool iecoOn = false;
  bool setOutSilent = false;
  bool outSilentOn = false;
  bool setSelfClean = false; // trigger only: always encodes [1]
  bool setBreezeAway = false;
  bool breezeAwayOn = false;
  bool setBreezeless = false;
  bool breezelessOn = false;
  bool setFlash = false;
  bool flashOn = false;
  bool setRateSelect = false;
  uint8_t rateSelectValue = 100; // raw gear byte; 100 = off
  bool setCascade = false;
  uint8_t cascadeMode = 0; // 0 off / 1 up / 2 down
  bool setFreshAir = false;
  uint8_t freshAirSpeed = 0; // 0 off / 40 / 60 / 80 / 100
  bool setBuzzer = false;
  bool buzzerOn = false; // PropertyId.BUZZER (0x001A); not readable back

  size_t serialize(uint8_t* out, size_t outCap) const;
};

} // namespace midea
