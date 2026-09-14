#include "Responses.h"

#include <cmath>
#include <initializer_list>

#include "Crc8.h"
#include "Frame.h"

namespace midea {
namespace {

// Tri-state helper: present values are kTrue/kFalse, kAbsent only before a
// record decodes the flag.
constexpr CapFlag flag(bool value) {
  return value ? CapFlag::kTrue : CapFlag::kFalse;
}

// any_of() from the Python capability readers.
constexpr bool anyOf(uint8_t value, std::initializer_list<uint8_t> candidates) {
  for (const uint8_t candidate : candidates) {
    if (candidate == value) return true;
  }
  return false;
}

// CapabilityId (command.py:29-95) entries with readers in the ported subset.
constexpr uint16_t kCapFanSpeedControl = 0x0210;
constexpr uint16_t kCapPresetEco = 0x0212;
constexpr uint16_t kCapPresetFreezeProtection = 0x0213;
constexpr uint16_t kCapModes = 0x0214;
constexpr uint16_t kCapSwingModes = 0x0215;
constexpr uint16_t kCapAnion = 0x021E;
constexpr uint16_t kCapHumidity = 0x021F;
constexpr uint16_t kCapFahrenheit = 0x0222;
constexpr uint16_t kCapTemperatures = 0x0225;
constexpr uint16_t kCapPresetTurbo = 0x021A;
constexpr uint16_t kCapPresetIeco = 0x00E3;
constexpr uint16_t kCapOutSilent = 0x00CD;
constexpr uint16_t kCapSelfClean = 0x0039;
constexpr uint16_t kCapBreezeAway = 0x0042;
constexpr uint16_t kCapBreezeless = 0x0018;
constexpr uint16_t kCapFlash = 0x0067;
constexpr uint16_t kCapRateSelect = 0x0048;
constexpr uint16_t kCapFreshAir = 0x004B;
constexpr uint16_t kCapCascade = 0x0059;
constexpr uint16_t kCapDisplayControl = 0x0224;

// StateResponse._parse_temperature (command.py:960-971). Python's int()
// truncates toward zero, hence std::trunc.
std::optional<double> parseTemperature(uint8_t data, double decimals, bool fahrenheit) {
  if (data == 0xFF) return std::nullopt;
  const double temperature = (static_cast<int>(data) - 50) / 2.0;
  const double integral = std::trunc(temperature);
  if (!fahrenheit && decimals != 0.0) {
    return integral + (temperature >= 0.0 ? decimals : -decimals);
  }
  if (decimals >= 0.5) {
    return integral + (temperature >= 0.0 ? 0.5 : -0.5);
  }
  return temperature;
}

} // namespace

ResponseKind classifyResponse(const uint8_t* frame, size_t len, uint8_t* groupOut) {
  if (groupOut != nullptr) *groupOut = 0;
  if (!validateFrame(frame, len, kDeviceTypeAc)) return ResponseKind::kInvalid;
  // Response.__init__ indexes payload[0] and the payload check needs a tail
  // byte, so anything under 13 bytes is rejected (Python raises).
  if (len < kFrameHeaderLen + 3) return ResponseKind::kInvalid;

  const uint8_t frameType = frame[9];
  const uint8_t responseId = frame[10];
  ResponseKind kind = ResponseKind::kNone;
  bool payloadCheckExempt = false;
  switch (responseId) {
    case kResponseIdState:
      kind = ResponseKind::kState;
      break;
    case kResponseIdCapabilities:
      // Devices also emit 0xB5 with frame type 0x5; ignore those (command.py:515).
      if (frameType != kFrameTypeQuery) return ResponseKind::kNone;
      kind = ResponseKind::kCapabilities;
      break;
    case kResponseIdProperties:
      kind = ResponseKind::kProperties;
      payloadCheckExempt = true;
      break;
    case kResponseIdPropertiesAck:
      kind = ResponseKind::kPropertiesAck;
      payloadCheckExempt = true;
      break;
    case kResponseIdGroupData:
      if (len < kFrameHeaderLen + 4) return ResponseKind::kInvalid; // group byte at frame[13]
      if (groupOut != nullptr) *groupOut = frame[13] & 0x0F;
      kind = ResponseKind::kGroupData;
      break;
    default:
      return ResponseKind::kNone;
  }

  // Response.validate (command.py:475-484): CRC8 or checksum over
  // frame[10:-2]; some devices use one, some the other.
  if (!payloadCheckExempt) {
    const uint8_t* payload = frame + kFrameHeaderLen;
    const size_t payloadLen = len - kFrameHeaderLen - 2;
    const uint8_t candidate = frame[len - 2];
    if (crc8(payload, payloadLen) != candidate && frameChecksum(payload, payloadLen) != candidate) {
      return ResponseKind::kInvalid;
    }
  }
  return kind;
}

bool parseCapabilitiesResponse(const uint8_t* payload, size_t len, AcCapabilities& out) {
  if (payload == nullptr || len < 2) return false;
  out = AcCapabilities{};

  // _parse_capabilities (command.py:655-727): count at byte 1, then count
  // variable-length records {id LE16, size, value[size]}.
  const uint8_t count = payload[1];
  const uint8_t* caps = payload + 2;
  size_t capsLen = len - 2;
  for (uint8_t i = 0; i < count; ++i) {
    if (capsLen < 3) break;
    const uint8_t size = caps[2];
    if (size == 0) {
      caps += 3;
      capsLen -= 3;
      continue;
    }
    // A record truncated by the payload end aborts the walk; Python slices
    // short and raises on the first value read.
    if (capsLen < static_cast<size_t>(3) + size) break;

    const uint16_t id = static_cast<uint16_t>(caps[0] | (static_cast<uint16_t>(caps[1]) << 8));
    const uint8_t value = caps[3];
    switch (id) {
      case kCapAnion:
        out.anion = flag(value == 1);
        break;
      case kCapPresetEco:
        out.eco = flag(anyOf(value, {1, 2}));
        break;
      case kCapPresetFreezeProtection:
        out.freezeProtection = flag(value == 1);
        break;
      case kCapFahrenheit:
        out.fahrenheit = flag(value == 0);
        break;
      case kCapModes:
        out.heatMode = flag(anyOf(value, {1, 2, 4, 6, 7, 9, 10, 11, 12, 13}));
        out.coolMode = flag(anyOf(value, {0, 1, 3, 4, 5, 6, 7, 8, 9, 11, 13, 14, 15}));
        out.dryMode = flag(anyOf(value, {0, 1, 5, 6, 9, 11, 13, 14, 15}));
        out.autoMode = flag(anyOf(value, {0, 1, 2, 7, 8, 9, 13, 14}));
        out.auxHeatMode = flag(value == 9);
        out.auxMode = flag(anyOf(value, {9, 10, 11, 13, 14, 15}));
        break;
      case kCapSwingModes:
        out.swingHorizontal = flag(anyOf(value, {1, 3}));
        out.swingVertical = flag(anyOf(value, {0, 1}));
        break;
      case kCapFanSpeedControl:
        out.fanSilent = flag(anyOf(value, {6, 9}));
        out.fanLow = flag(anyOf(value, {3, 4, 5, 6, 7, 9}));
        out.fanMedium = flag(anyOf(value, {5, 6, 7}));
        out.fanHigh = flag(anyOf(value, {3, 4, 5, 6, 7, 9}));
        out.fanAuto = flag(anyOf(value, {4, 5, 6, 9}));
        out.fanCustom = flag(value == 1);
        break;
      case kCapHumidity:
        out.humidityAutoSet = flag(anyOf(value, {1, 2}));
        out.humidityManualSet = flag(anyOf(value, {2, 3}));
        break;
      case kCapPresetTurbo:
        out.turboHeat = flag(anyOf(value, {1, 3}));
        out.turboCool = flag(anyOf(value, {0, 1}));
        break;
      case kCapPresetIeco: {
        // Readers read v[0] and v[1] if present (command.py:615-620); the
        // support/level rules live in CapabilitiesResponse.ieco/ieco_number.
        const uint8_t iecoEnd = size >= 2 ? caps[4] : 0;
        out.ieco = flag(anyOf(value, {1, 3, 4, 8}) || anyOf(iecoEnd, {1, 2, 3, 8}));
        out.iecoNumber = iecoEnd == 8 ? 8 : (anyOf(iecoEnd, {1, 2, 3}) ? 3 : 1);
        break;
      }
      case kCapOutSilent:
        out.outSilent = flag(anyOf(value, {1, 3}));
        break;
      case kCapSelfClean:
        out.selfClean = flag(value == 1);
        break;
      case kCapBreezeAway:
        out.breezeAway = flag(value == 1);
        break;
      case kCapBreezeless:
        out.breezeless = flag(value == 1);
        break;
      case kCapFlash:
        out.flash = flag(anyOf(value, {1, 2, 3, 4}));
        break;
      case kCapRateSelect:
        // msmart rate_select_levels(): gear flag is 2-level, {2,3} is 5-level.
        out.rateSelect2Level = flag(value == 1);
        out.rateSelect5Level = flag(anyOf(value, {2, 3}));
        break;
      case kCapCascade:
        out.cascade = flag(value == 1);
        break;
      case kCapFreshAir:
        out.freshAir = flag(value == 1);
        break;
      case kCapDisplayControl:
        out.displayControl = flag(anyOf(value, {1, 2, 100}));
        break;
      case kCapTemperatures:
        // Python `continue`s here without advancing past the record; mirrored.
        if (size < 6) continue;
        out.hasTemperatureRanges = true;
        out.coolMin = caps[3] * 0.5;
        out.coolMax = caps[4] * 0.5;
        out.autoMin = caps[5] * 0.5;
        out.autoMax = caps[6] * 0.5;
        out.heatMin = caps[7] * 0.5;
        out.heatMax = caps[8] * 0.5;
        out.temperatureDecimals = (size > 6 ? caps[9] : caps[2]) != 0;
        break;
      default:
        // Known-but-not-ported and unknown ids: skip the record byte-exactly.
        break;
    }
    caps += 3 + size;
    capsLen -= 3 + size;
  }

  // "More capabilities" byte: the second-to-last byte of the remainder.
  if (capsLen > 1) out.additionalPending = caps[capsLen - 2] != 0;
  return true;
}

void mergeCapabilities(AcCapabilities& dst, const AcCapabilities& src) {
  // dict.update semantics: only keys present in src overwrite dst.
  const auto merge = [](CapFlag& dest, CapFlag from) {
    if (from != CapFlag::kAbsent) dest = from;
  };
  merge(dst.anion, src.anion);
  merge(dst.eco, src.eco);
  merge(dst.freezeProtection, src.freezeProtection);
  merge(dst.fahrenheit, src.fahrenheit);
  merge(dst.heatMode, src.heatMode);
  merge(dst.coolMode, src.coolMode);
  merge(dst.dryMode, src.dryMode);
  merge(dst.autoMode, src.autoMode);
  merge(dst.auxHeatMode, src.auxHeatMode);
  merge(dst.auxMode, src.auxMode);
  merge(dst.swingHorizontal, src.swingHorizontal);
  merge(dst.swingVertical, src.swingVertical);
  merge(dst.fanSilent, src.fanSilent);
  merge(dst.fanLow, src.fanLow);
  merge(dst.fanMedium, src.fanMedium);
  merge(dst.fanHigh, src.fanHigh);
  merge(dst.fanAuto, src.fanAuto);
  merge(dst.fanCustom, src.fanCustom);
  merge(dst.humidityAutoSet, src.humidityAutoSet);
  merge(dst.humidityManualSet, src.humidityManualSet);
  merge(dst.turboHeat, src.turboHeat);
  merge(dst.turboCool, src.turboCool);
  merge(dst.ieco, src.ieco);
  // The level count only travels with its record; absent src.ieco means the
  // record was never parsed, so keep the previous number.
  if (src.ieco != CapFlag::kAbsent) dst.iecoNumber = src.iecoNumber;
  merge(dst.outSilent, src.outSilent);
  merge(dst.selfClean, src.selfClean);
  merge(dst.breezeAway, src.breezeAway);
  merge(dst.breezeless, src.breezeless);
  merge(dst.flash, src.flash);
  merge(dst.rateSelect2Level, src.rateSelect2Level);
  merge(dst.rateSelect5Level, src.rateSelect5Level);
  merge(dst.cascade, src.cascade);
  merge(dst.freshAir, src.freshAir);
  merge(dst.displayControl, src.displayControl);
  if (src.hasTemperatureRanges) {
    dst.hasTemperatureRanges = true;
    dst.coolMin = src.coolMin;
    dst.coolMax = src.coolMax;
    dst.autoMin = src.autoMin;
    dst.autoMax = src.autoMax;
    dst.heatMin = src.heatMin;
    dst.heatMax = src.heatMax;
    dst.temperatureDecimals = src.temperatureDecimals;
  }
}

bool parseStateResponse(const uint8_t* payload, size_t len, AcState& out) {
  if (payload == nullptr || len < 17) return false;
  out = AcState{};
  const uint8_t* p = payload;

  out.powerOn = (p[1] & 0x01) != 0;

  out.targetTemperature = static_cast<double>(p[2] & 0x0F) + 16.0;
  if (p[2] & 0x10) out.targetTemperature += 0.5;
  out.operationalMode = (p[2] >> 5) & 0x07;

  out.fanSpeed = p[3] & 0x7F;
  out.swingMode = p[7] & 0x0F;

  out.turbo = (p[8] & 0x20) != 0;
  out.independentAuxHeat = (p[8] & 0x40) != 0;
  out.followMe = (p[8] & 0x80) != 0;

  out.eco = (p[9] & 0x10) != 0;
  out.purifier = (p[9] & 0x20) != 0;
  out.auxHeat = (p[9] & 0x08) != 0;

  out.sleep = (p[10] & 0x01) != 0;
  out.turbo = out.turbo || (p[10] & 0x02) != 0;
  out.fahrenheit = (p[10] & 0x04) != 0;

  out.indoorTemperature = parseTemperature(p[11], static_cast<double>(p[15] & 0x0F) / 10.0, out.fahrenheit);
  out.outdoorTemperature = parseTemperature(p[12], static_cast<double>(p[15] >> 4) / 10.0, out.fahrenheit);

  // Alternate target temperature overrides the primary nibble when non-zero;
  // the half-degree bit still comes from byte 2.
  const uint8_t targetAlt = p[13] & 0x1F;
  if (targetAlt != 0) {
    out.targetTemperature = static_cast<double>(targetAlt) + 12.0;
    if (p[2] & 0x10) out.targetTemperature += 0.5;
  }
  out.filterAlert = (p[13] & 0x20) != 0;

  out.displayOn = p[14] != 0x70;
  out.errorCode = p[16];

  if (len >= 20) out.targetHumidity = static_cast<uint8_t>(p[19] & 0x7F);
  if (len >= 22) out.freezeProtection = (p[21] & 0x80) != 0;
  return true;
}

namespace {

// PropertyId (command.py:96-115) entries decoded by parsePropertyResponse.
constexpr uint16_t kPropIeco = 0x00E3;
constexpr uint16_t kPropOutSilent = 0x00CD;
constexpr uint16_t kPropSelfClean = 0x0039;
constexpr uint16_t kPropBreezeAway = 0x0042;
constexpr uint16_t kPropBreezeless = 0x0018;
constexpr uint16_t kPropFlash = 0x0067;
constexpr uint16_t kPropRateSelect = 0x0048;
constexpr uint16_t kPropCascade = 0x0059;
constexpr uint16_t kPropFreshAir = 0x004B;
constexpr uint16_t kPropBuzzer = 0x001A;

} // namespace

bool parsePropertyResponse(const uint8_t* payload, size_t len, AcPropertyValues& out) {
  out = AcPropertyValues{};
  if (payload == nullptr || len < 2) return false;

  // PropertiesResponse._parse (command.py:1079-1119): count at byte 1, then
  // TLVs {id LE16, result, len, value}. The result bit 0x10 flags a failed
  // execution; upstream logs it and still decodes the value, mirrored here.
  const uint8_t count = payload[1];
  const uint8_t* p = payload + 2;
  size_t remaining = len - 2;
  for (uint8_t i = 0; i < count; ++i) {
    if (remaining < 4) break;
    const uint8_t size = p[3];
    if (size == 0) {
      p += 4;
      remaining -= 4;
      continue;
    }
    // Truncated value: Python slices short and decode() raises; stop walking.
    if (remaining < static_cast<size_t>(4) + size) break;

    const uint16_t id = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
    const bool failed = (p[2] & 0x10) != 0;
    if (id == kPropIeco && size >= 2) {
      // IECO decode: data[0] is the level count, data[1] the switch.
      out.hasIeco = true;
      out.iecoOn = p[5] != 0;
      out.iecoFailed = failed;
    } else if (id == kPropOutSilent) {
      // OUT_SILENT decode: 3 means on.
      out.hasOutSilent = true;
      out.outSilentOn = p[4] == 3;
      out.outSilentFailed = failed;
    } else if (id == kPropSelfClean) {
      // SELF_CLEAN decode: bool(data[0]) = self-clean cycle running.
      out.hasSelfClean = true;
      out.selfCleanOn = p[4] != 0;
      out.selfCleanFailed = failed;
    } else if (id == kPropBreezeAway) {
      // BREEZE_AWAY decode: only 2 means on (1 is the explicit off value).
      out.hasBreezeAway = true;
      out.breezeAwayOn = p[4] == 2;
      out.breezeAwayFailed = failed;
    } else if (id == kPropBreezeless) {
      out.hasBreezeless = true;
      out.breezelessOn = p[4] != 0;
      out.breezelessFailed = failed;
    } else if (id == kPropFlash) {
      out.hasFlash = true;
      out.flashOn = p[4] != 0;
      out.flashFailed = failed;
    } else if (id == kPropRateSelect) {
      // RATE_SELECT decode: the gear byte itself.
      out.hasRateSelect = true;
      out.rateSelect = p[4];
      out.rateSelectFailed = failed;
    } else if (id == kPropCascade && size >= 2) {
      // CASCADE decode: data[0] ? data[1] : 0 (mode byte when enabled).
      out.hasCascade = true;
      out.cascade = p[4] != 0 ? p[5] : 0;
      out.cascadeFailed = failed;
    } else if (id == kPropFreshAir && size >= 2) {
      // FRESH_AIR decode: data[0] ? data[1] : 0 (fan speed when powered).
      out.hasFreshAir = true;
      out.freshAir = p[4] != 0 ? p[5] : 0;
      out.freshAirFailed = failed;
    }
    p += 4 + size;
    remaining -= 4 + size;
  }
  return true;
}

bool parsePropertyAck(const uint8_t* payload, size_t len, AcPropertyAck& out) {
  out = AcPropertyAck{};
  if (payload == nullptr || len < 2) return false;

  // SetPropertiesCommand ack (0xB0): [id LE16, result, len, value]. The result
  // bit 0x10 flags a failed execution. Unlike the query parse, the result byte
  // is read before the size check so a bare len-0 nack still reports failure.
  const uint8_t count = payload[1];
  const uint8_t* p = payload + 2;
  size_t remaining = len - 2;
  for (uint8_t i = 0; i < count; ++i) {
    if (remaining < 4) break;
    const uint16_t id = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
    const bool failed = (p[2] & 0x10) != 0;
    const uint8_t size = p[3];
    if (id == kPropIeco) {
      out.sawIeco = true;
      out.iecoFailed = failed;
    } else if (id == kPropOutSilent) {
      out.sawOutSilent = true;
      out.outSilentFailed = failed;
    } else if (id == kPropSelfClean) {
      out.sawSelfClean = true;
      out.selfCleanFailed = failed;
    } else if (id == kPropBreezeAway) {
      out.sawBreezeAway = true;
      out.breezeAwayFailed = failed;
    } else if (id == kPropBreezeless) {
      out.sawBreezeless = true;
      out.breezelessFailed = failed;
    } else if (id == kPropFlash) {
      out.sawFlash = true;
      out.flashFailed = failed;
    } else if (id == kPropRateSelect) {
      out.sawRateSelect = true;
      out.rateSelectFailed = failed;
    } else if (id == kPropCascade) {
      out.sawCascade = true;
      out.cascadeFailed = failed;
    } else if (id == kPropFreshAir) {
      out.sawFreshAir = true;
      out.freshAirFailed = failed;
    } else if (id == kPropBuzzer) {
      out.sawBuzzer = true;
      out.buzzerFailed = failed;
    }
    // Truncated value: the TLV is the last one Python could read; stop walking.
    if (remaining < static_cast<size_t>(4) + size) break;
    p += 4 + size;
    remaining -= 4 + size;
  }
  return true;
}

namespace {

// decode_bcd (Group4Response._parse): per-nibble 10*(d >> 4) + (d & 0xF);
// non-BCD nibbles (0xA..0xF) decode past 9 exactly like upstream does.
constexpr unsigned decodeBcd(uint8_t d) {
  return 10u * (d >> 4) + (d & 0x0Fu);
}

// parse_energy/parse_power (Group4Response._parse): d points at 4 (energy) or
// 3 (power) bytes; fills both the BCD and the binary decode.
void decodeEnergy(const uint8_t* d, double& bcd, double& binary) {
  bcd = 10000.0 * decodeBcd(d[0]) + 100.0 * decodeBcd(d[1]) + static_cast<double>(decodeBcd(d[2])) +
        0.01 * decodeBcd(d[3]);
  binary = ((static_cast<uint32_t>(d[0]) << 24) | (static_cast<uint32_t>(d[1]) << 16) |
            (static_cast<uint32_t>(d[2]) << 8) | d[3]) /
           10.0;
}

void decodePower(const uint8_t* d, double& bcd, double& binary) {
  bcd = 1000.0 * decodeBcd(d[0]) + 10.0 * decodeBcd(d[1]) + 0.1 * decodeBcd(d[2]);
  binary = ((static_cast<uint32_t>(d[0]) << 16) | (static_cast<uint32_t>(d[1]) << 8) | d[2]) / 10.0;
}

} // namespace

bool parseGroup1Response(const uint8_t* payload, size_t len, AcExtStats& out) {
  if (payload == nullptr || len < 15) return false; // highest offset: p[14] (TP)
  const uint8_t* p = payload;

  out.compressorFrequencyHz = p[4];
  out.compressorTargetFrequencyHz = p[5];
  out.compressorCurrent = p[7];
  out.compressorVoltageV = p[8];
  // T1/T2 offset -30, T3/T4 offset -50, half-degree steps (Group1Response).
  out.t1IndoorAmbientC = (static_cast<int>(p[10]) - 30) / 2.0;
  out.t2IndoorCoilC = (static_cast<int>(p[11]) - 30) / 2.0;
  out.t3OutdoorCoilC = (static_cast<int>(p[12]) - 50) / 2.0;
  out.t4OutdoorAmbientC = (static_cast<int>(p[13]) - 50) / 2.0;
  out.dischargePipeC = p[14];
  return true;
}

bool parseGroup2Response(const uint8_t* payload, size_t len, AcExtStats& out) {
  if (payload == nullptr || len < 9) return false; // highest offset: p[8] (pump)
  const uint8_t* p = payload;

  // Raw value * 8 is the RPM-equivalent unit upstream reports.
  out.indoorFanTargetRpm = static_cast<uint16_t>(p[4]) * 8u;
  out.indoorFanRpm = static_cast<uint16_t>(p[5]) * 8u;
  out.waterPumpRunning = (p[8] & 0x10) != 0;
  return true;
}

bool parseGroup4Response(const uint8_t* payload, size_t len, AcExtStats& out) {
  if (payload == nullptr || len < 19) return false; // highest offset: p[18] (power)
  const uint8_t* p = payload;

  double totalBcd = 0.0, totalBin = 0.0, runBcd = 0.0, runBin = 0.0, powerBcd = 0.0, powerBin = 0.0;
  decodeEnergy(p + 4, totalBcd, totalBin); // total, bytes 4-7
  decodeEnergy(p + 12, runBcd, runBin);    // current run, bytes 12-15
  decodePower(p + 16, powerBcd, powerBin); // real-time power, bytes 16-18

  // "Energy monitoring is valid if at least one stat is non-zero" — the BCD
  // forms decide, matching upstream. An all-zero answer leaves the previous
  // snapshot untouched (accumulating-parser semantics vs Python's fresh None).
  if (totalBcd == 0.0 && runBcd == 0.0 && powerBcd == 0.0) return true;

  out.totalEnergyKwhBcd = totalBcd;
  out.runEnergyKwhBcd = runBcd;
  out.realTimePowerWBcd = powerBcd;
  out.totalEnergyKwhBinary = totalBin;
  out.runEnergyKwhBinary = runBin;
  out.realTimePowerWBinary = powerBin;
  return true;
}

bool parseGroup5Response(const uint8_t* payload, size_t len, AcExtStats& out) {
  if (payload == nullptr || len < 11) return false; // highest offset: p[10] (defrost)
  const uint8_t* p = payload;

  // Humidity 0 means "no sensor": an answer always overwrites the previous
  // value, including clearing it back to nullopt.
  if (p[4] != 0) {
    out.humidityPercent = p[4];
  } else {
    out.humidityPercent = std::nullopt;
  }
  out.outdoorFanRpm = static_cast<uint16_t>(p[8]) * 8u;
  out.defrost = p[10] != 0;
  return true;
}

bool parseGroup7Response(const uint8_t* payload, size_t len, AcExtStats& out) {
  if (payload == nullptr || len < 12) return false; // highest offset: p[11]
  const uint8_t* p = payload;

  out.outdoorUnitPowerW = static_cast<uint16_t>(p[10]) + 256u * p[11];
  return true;
}

bool parseGroup11Response(const uint8_t* payload, size_t len, AcExtStats& out) {
  if (payload == nullptr || len < 13) return false; // highest offset: p[12]
  const uint8_t* p = payload;

  out.horizontalLouverDeg = p[9];
  out.verticalLouverDeg = p[12];
  return true;
}

bool parseGroupData(uint8_t group, const uint8_t* payload, size_t len, AcExtStats& out) {
  switch (group) {
    case 1:
      return parseGroup1Response(payload, len, out);
    case 2:
      return parseGroup2Response(payload, len, out);
    case 4:
      return parseGroup4Response(payload, len, out);
    case 5:
      return parseGroup5Response(payload, len, out);
    case 7:
      return parseGroup7Response(payload, len, out);
    case 11:
      return parseGroup11Response(payload, len, out);
    default:
      return false; // no parser for this group
  }
}

} // namespace midea
