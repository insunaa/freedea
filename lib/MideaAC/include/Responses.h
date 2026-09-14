#pragma once

// Midea AC response dispatch plus StateResponse, CapabilitiesResponse and
// group-data (0xC1) parsers: msmart command.py:455-531 (Response.construct),
// 931-1066 (StateResponse), 532-930 (CapabilitiesResponse, decoded subset per
// docs/porting-notes.md sec. 6) and 1130-1341 (Group1/2/4/5/7/11Response)
// at d7db53b.

#include <cstddef>
#include <cstdint>

#include "AcExtStats.h"
#include "AcState.h"

namespace midea {

// ResponseId (command.py:21-27).
constexpr uint8_t kResponseIdPropertiesAck = 0xB0;
constexpr uint8_t kResponseIdProperties = 0xB1;
constexpr uint8_t kResponseIdCapabilities = 0xB5;
constexpr uint8_t kResponseIdState = 0xC0;
constexpr uint8_t kResponseIdGroupData = 0xC1;

// Outcome of Response.construct's frame dispatch. kInvalid means the frame or
// its payload CRC failed validation; kNone is a recognized frame with no
// parser (unknown id, or the unsolicited 0xB5 with frame type 0x5 that some
// devices emit). Properties and group data are classified but parsed later.
enum class ResponseKind : uint8_t {
  kInvalid,
  kNone,
  kState,
  kCapabilities,
  kProperties,
  kPropertiesAck,
  kGroupData,
};

// Mirrors Response.construct steps 1-4 (command.py:486-531): validate the
// frame, dispatch on the response id at frame[10] (capabilities additionally
// require a QUERY frame type), then the payload check — CRC8 or frame
// checksum over frame[10:-2] must equal frame[-2], except for properties
// which some devices send with bad CRCs. groupOut, when non-null, receives
// frame[13] & 0xF for kGroupData and 0 otherwise (group selects the concrete
// group parser). The parsed payload for the parsers below is frame + 10 with
// length len - 12.
ResponseKind classifyResponse(const uint8_t* frame, size_t len, uint8_t* groupOut = nullptr);

// Tri-state capability flag: Python's _capabilities dict distinguishes a key
// that was never received from one decoded as False, and merge() only copies
// keys that are present.
enum class CapFlag : uint8_t { kAbsent = 0, kFalse = 1, kTrue = 2 };

// The subset of CapabilitiesResponse._capabilities the UI needs. Capability
// records outside this subset (including all unknown ids) are walked past
// byte-exactly but not decoded.
struct AcCapabilities {
  CapFlag anion = CapFlag::kAbsent;
  CapFlag eco = CapFlag::kAbsent;
  CapFlag freezeProtection = CapFlag::kAbsent;
  CapFlag fahrenheit = CapFlag::kAbsent;
  CapFlag heatMode = CapFlag::kAbsent;
  CapFlag coolMode = CapFlag::kAbsent;
  CapFlag dryMode = CapFlag::kAbsent;
  CapFlag autoMode = CapFlag::kAbsent;
  CapFlag auxHeatMode = CapFlag::kAbsent;
  CapFlag auxMode = CapFlag::kAbsent;
  CapFlag swingHorizontal = CapFlag::kAbsent;
  CapFlag swingVertical = CapFlag::kAbsent;
  CapFlag fanSilent = CapFlag::kAbsent;
  CapFlag fanLow = CapFlag::kAbsent;
  CapFlag fanMedium = CapFlag::kAbsent;
  CapFlag fanHigh = CapFlag::kAbsent;
  CapFlag fanAuto = CapFlag::kAbsent;
  CapFlag fanCustom = CapFlag::kAbsent;
  CapFlag humidityAutoSet = CapFlag::kAbsent;
  CapFlag humidityManualSet = CapFlag::kAbsent;
  CapFlag turboHeat = CapFlag::kAbsent;
  CapFlag turboCool = CapFlag::kAbsent;

  // iECO (0x00E3 record [ieco, ieco_end]; supported iff ieco in {1,3,4,8} or
  // ieco_end in {1,2,3,8}) and PortaSplit outdoor silent (0x00CD, value in
  // {1,3}). iecoNumber is the level count derived from ieco_end (8 ECOMaster,
  // 3, else 1); only meaningful when ieco != kAbsent and merged together
  // with the flag, mirroring the record's presence.
  CapFlag ieco = CapFlag::kAbsent;
  CapFlag outSilent = CapFlag::kAbsent;
  uint8_t iecoNumber = 1;

  // Property-channel feature caps (8.5): SELF_CLEAN 0x0039 (value == 1),
  // BREEZE_AWAY 0x0042 (value == 1), BREEZELESS 0x0018 (value == 1) and
  // FLASH 0x0067 (value in {1,2,3,4}).
  CapFlag selfClean = CapFlag::kAbsent;
  CapFlag breezeAway = CapFlag::kAbsent;
  CapFlag breezeless = CapFlag::kAbsent;
  CapFlag flash = CapFlag::kAbsent;

  // Enum-feature property caps (8.6): RATE_SELECT 0x0048 (2-level gear flag
  // value == 1, 5-level flag value in {2,3}; msmart rate_select_levels),
  // CASCADE 0x0059 and FRESH_AIR 0x004B (both value == 1).
  CapFlag rateSelect2Level = CapFlag::kAbsent;
  CapFlag rateSelect5Level = CapFlag::kAbsent;
  CapFlag cascade = CapFlag::kAbsent;
  CapFlag freshAir = CapFlag::kAbsent;

  // DISPLAY_CONTROL 0x0224 (value in {1,2,100}): the device accepts the
  // ToggleDisplay command for its LED display (8.7).
  CapFlag displayControl = CapFlag::kAbsent;

  // TEMPERATURES (0x0225); the range fields are valid only when set.
  bool hasTemperatureRanges = false;
  double coolMin = 0.0;
  double coolMax = 0.0;
  double autoMin = 0.0;
  double autoMax = 0.0;
  double heatMin = 0.0;
  double heatMax = 0.0;
  bool temperatureDecimals = false;

  // "More capabilities pending" byte after the last record; requests the rest
  // with GetCapabilitiesCommand(additional=true). mergeCapabilities leaves it
  // alone, matching Python's merge().
  bool additionalPending = false;
};

// Parses a CapabilitiesResponse payload (frame[10:-2]; byte 0 is the response
// id, the record count is byte 1). out is reset before parsing. Returns false
// only when the payload cannot hold a count byte.
bool parseCapabilitiesResponse(const uint8_t* payload, size_t len, AcCapabilities& out);

// CapabilitiesResponse.merge (command.py:738-740): src overwrites dst only
// where src has a value; the temperature block replaces as a unit.
void mergeCapabilities(AcCapabilities& dst, const AcCapabilities& src);

// Parses a StateResponse payload (frame[10:-2]; byte 0 is the response id).
// Returns false when shorter than 17 bytes (Python reads payload[16]
// unconditionally). out is reset before parsing.
bool parseStateResponse(const uint8_t* payload, size_t len, AcState& out);

// Decoded values from a PropertiesResponse (0xB1 get answer / 0xB0 set ack).
// The has* flags distinguish "TLV absent" from a decoded false; *Failed
// mirrors the result bit 0x10, which upstream only logs — the value is still
// decoded alongside it. Repeated ids follow dict semantics: last one wins.
struct AcPropertyValues {
  bool hasIeco = false;
  bool iecoOn = false;
  bool iecoFailed = false;
  bool hasOutSilent = false;
  bool outSilentOn = false;
  bool outSilentFailed = false;
  // 8.5 booleans: SELF_CLEAN decodes "cycle running", BREEZE_AWAY data[0] == 2,
  // BREEZELESS/FLASH plain bool(data[0]).
  bool hasSelfClean = false;
  bool selfCleanOn = false;
  bool selfCleanFailed = false;
  bool hasBreezeAway = false;
  bool breezeAwayOn = false;
  bool breezeAwayFailed = false;
  bool hasBreezeless = false;
  bool breezelessOn = false;
  bool breezelessFailed = false;
  bool hasFlash = false;
  bool flashOn = false;
  bool flashFailed = false;
  // 8.6 enum properties, decoded protocol-native: rate select gear byte
  // (100 off; 2-level {50, 75}; 5-level {1, 20, 40, 60, 80}), cascade mode
  // (0 off / 1 up / 2 down, data[0] ? data[1] : 0) and fresh-air fan speed
  // (0 off / 40 / 60 / 80 / 100, data[0] ? data[1] : 0).
  bool hasRateSelect = false;
  uint8_t rateSelect = 0;
  bool rateSelectFailed = false;
  bool hasCascade = false;
  uint8_t cascade = 0;
  bool cascadeFailed = false;
  bool hasFreshAir = false;
  uint8_t freshAir = 0;
  bool freshAirFailed = false;
};

// Parses a properties response payload (frame[10:-2]; byte 0 is the response
// id, byte 1 the TLV count). TLVs are [id LE16, result, len, value]; zero-
// length TLVs are skipped. A TLV truncated by the payload end (where Python
// raises IndexError) stops the walk; its value is not decoded. out is reset
// before parsing. Returns false only when the payload cannot hold id + count.
bool parsePropertyResponse(const uint8_t* payload, size_t len, AcPropertyValues& out);

// Per-property result bits from a 0xB0 set ack. Unlike parsePropertyResponse
// (which skips zero-length TLVs), the result byte is read even for a bare
// len-0 ack, because a nack carries the failure there and no value. Records
// the result bit 0x10 per known id; repeated ids follow last-wins.
struct AcPropertyAck {
  bool sawIeco = false;
  bool iecoFailed = false;
  bool sawOutSilent = false;
  bool outSilentFailed = false;
  bool sawSelfClean = false;
  bool selfCleanFailed = false;
  bool sawBreezeAway = false;
  bool breezeAwayFailed = false;
  bool sawBreezeless = false;
  bool breezelessFailed = false;
  bool sawFlash = false;
  bool flashFailed = false;
  bool sawRateSelect = false;
  bool rateSelectFailed = false;
  bool sawCascade = false;
  bool cascadeFailed = false;
  bool sawFreshAir = false;
  bool freshAirFailed = false;
  // BUZZER (0x001A): ack-visible only (the value is not decodable, msmart
  // returns None for it), so it exists purely for the set/ack log.
  bool sawBuzzer = false;
  bool buzzerFailed = false;
};

// Parses a set-ack payload ([id LE16, result, len, value] TLVs). Same frame
// shape as parsePropertyResponse but reports failure for zero-length TLVs
// too. Truncated value stops the walk; out is reset before parsing; false
// only when the payload cannot hold id + count.
bool parsePropertyAck(const uint8_t* payload, size_t len, AcPropertyAck& out);

// Group-data (0xC1) parsers. Unlike parseStateResponse these ACCUMULATE:
// each parser writes only its own group's fields of the shared AcExtStats
// snapshot and leaves everything else untouched. `payload` is frame[10:-2]
// (byte 3 carries 0x40|group). Each returns false — writing nothing — when
// the payload is shorter than the group's highest accessed offset; there is
// no length-conditional field semantics here (upstream indexes unguarded).
bool parseGroup1Response(const uint8_t* payload, size_t len, AcExtStats& out);
bool parseGroup2Response(const uint8_t* payload, size_t len, AcExtStats& out);
bool parseGroup4Response(const uint8_t* payload, size_t len, AcExtStats& out);
bool parseGroup5Response(const uint8_t* payload, size_t len, AcExtStats& out);
bool parseGroup7Response(const uint8_t* payload, size_t len, AcExtStats& out);
bool parseGroup11Response(const uint8_t* payload, size_t len, AcExtStats& out);

// Dispatches on the group number (classifyResponse's groupOut; 1/2/4/5/7/11
// have parsers, anything else returns false).
bool parseGroupData(uint8_t group, const uint8_t* payload, size_t len, AcExtStats& out);

} // namespace midea
