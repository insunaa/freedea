#include "Commands.h"

#include <cmath>
#include <cstring>

#include "Crc8.h"

namespace midea {
namespace {

// Command._message_id (command.py:190). Single writer: the comm task.
uint8_t gMessageId = 0;

constexpr size_t kMaxCommandDataLen = kMaxCommandData;

// Command.tobytes (command.py:195-201): payload = data + [message_id],
// CRC8 over that whole payload, then Frame.tobytes wraps it.
size_t commandFrame(uint8_t* out, size_t outCap, uint8_t frameType, const uint8_t* data, size_t dataLen) {
  if (dataLen > kMaxCommandDataLen) return 0;
  uint8_t payload[kMaxCommandDataLen + 2];
  std::memcpy(payload, data, dataLen);
  payload[dataLen] = nextMessageId();
  payload[dataLen + 1] = crc8(payload, dataLen + 1);
  return buildFrame(out, outCap, kDeviceTypeAc, frameType, payload, dataLen + 2);
}

} // namespace

uint8_t nextMessageId() {
  gMessageId = static_cast<uint8_t>(gMessageId + 1); // & 0xFF by truncation
  return gMessageId;
}

void resetMessageId(uint8_t value) {
  gMessageId = value;
}

size_t GetStateCommand::serialize(uint8_t* out, size_t outCap) const {
  uint8_t data[kGetStateDataLen] = {
      0x41, 0x81, 0x00, 0xFF, 0x03, 0xFF, 0x00, temperatureType, 0x00, 0x00, 0x00, 0x00, //
      0x00, 0x00, 0x00, 0x00,                                                            //
      0x00, 0x00, 0x00, 0x00,                                                            //
      0x03,
  };
  static_assert(sizeof(data) == kGetStateDataLen, "GetState data length");
  return commandFrame(out, outCap, kFrameTypeQuery, data, sizeof(data));
}

size_t SetStateCommand::serialize(uint8_t* out, size_t outCap) const {
  // Target temperature encoding (command.py:300-315): primary 17..30 in the
  // low nibble, otherwise alternate field (int - 12) & 0x1F (C++20 two's
  // complement matches Python's & on negatives); 0x10 marks a half degree.
  double integral = 0.0;
  const double fractional = std::modf(targetTemperature, &integral);
  const int32_t tempInt = static_cast<int32_t>(integral);
  uint8_t temperature = 0;
  uint8_t temperatureAlt = 0;
  if (tempInt >= 17 && tempInt <= 30) {
    temperature = static_cast<uint8_t>((tempInt - 16) & 0x0F);
  } else {
    temperatureAlt = static_cast<uint8_t>((tempInt - 12) & 0x1F);
  }
  if (fractional > 0.0) temperature |= 0x10;

  uint8_t data[kSetStateDataLen] = {};
  data[0] = 0x40; // Set state
  // Byte 1: beep and power; CONTROL_SOURCE is always set.
  data[1] = static_cast<uint8_t>(kControlSource | (beepOn ? 0x40 : 0) | (powerOn ? 0x01 : 0));
  // Byte 2: temperature | (mode & 0x7) << 5.
  data[2] = static_cast<uint8_t>(temperature | ((operationalMode & 0x07) << 5));
  data[3] = fanSpeed;
  data[4] = 0x7F; // timer, always off (bytes 4-6)
  data[5] = 0x7F;
  data[7] = static_cast<uint8_t>(0x30 | (swingMode & 0x3F));
  // Turbo is sent twice: here (0x20) and in byte 10 (0x02).
  data[8] = static_cast<uint8_t>((followMe ? 0x80 : 0) | (turbo ? 0x20 : 0));
  data[9] =
      static_cast<uint8_t>((eco ? 0x80 : 0) | (purifier ? 0x20 : 0) | (forceAuxHeat ? 0x10 : 0) | (auxHeat ? 0x08 : 0));
  data[10] = static_cast<uint8_t>((sleep ? 0x01 : 0) | (turbo ? 0x02 : 0) | (fahrenheit ? 0x04 : 0));
  data[18] = temperatureAlt;
  data[19] = static_cast<uint8_t>(targetHumidity & 0x7F);
  data[21] = freezeProtection ? 0x80 : 0;
  data[22] = independentAuxHeat ? 0x08 : 0;
  static_assert(sizeof(data) == kSetStateDataLen, "SetState data length");
  return commandFrame(out, outCap, kFrameTypeControl, data, sizeof(data));
}

size_t GetCapabilitiesCommand::serialize(uint8_t* out, size_t outCap) const {
  const uint8_t data[] = {0xB5, 0x01, static_cast<uint8_t>(additional ? 0x01 : 0x00), 0x01};
  const size_t dataLen = additional ? 4 : 3;
  return commandFrame(out, outCap, kFrameTypeQuery, data, dataLen);
}

size_t GetGroupDataCommand::serialize(uint8_t* out, size_t outCap) const {
  const uint8_t data[kGetGroupDataDataLen] = {
      0x41,
      0x21,
      0x01,
      static_cast<uint8_t>(0x40 | (group & 0x0F)),
  };
  static_assert(sizeof(data) == kGetGroupDataDataLen, "GetGroupData data length");
  return commandFrame(out, outCap, kFrameTypeQuery, data, sizeof(data));
}

size_t ToggleDisplayCommand::serialize(uint8_t* out, size_t outCap) const {
  // Same 0x41 query preamble as GetState; byte 1 carries the beep bit (0x40)
  // and control source (command.py:395-407).
  uint8_t data[kToggleDisplayDataLen] = {
      0x41, static_cast<uint8_t>(kControlSource | (beepOn ? 0x40 : 0x00)), 0x00, 0xFF, 0x02, 0x00, 0x02,
  };
  static_assert(sizeof(data) == kToggleDisplayDataLen, "ToggleDisplay data length");
  return commandFrame(out, outCap, kFrameTypeQuery, data, sizeof(data));
}

namespace {

// PropertyId (command.py:96-115) entries sent by the property commands.
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
constexpr size_t kIecoValueLen = 13;

// Worst-case properties payload: header + iECO TLV + six 1-byte TLVs. Must
// fit the commandFrame scratch sized by kMaxCommandDataLen.
static_assert(kPropertiesMaxDataLen <= kMaxCommandDataLen, "properties data exceeds command scratch");

} // namespace

size_t GetPropertiesCommand::serialize(uint8_t* out, size_t outCap) const {
  const uint8_t count =
      static_cast<uint8_t>((queryIeco ? 1 : 0) + (queryOutSilent ? 1 : 0) + (querySelfClean ? 1 : 0) +
                           (queryBreezeAway ? 1 : 0) + (queryBreezeless ? 1 : 0) + (queryFlash ? 1 : 0) +
                           (queryRateSelect ? 1 : 0) + (queryCascade ? 1 : 0) + (queryFreshAir ? 1 : 0));
  if (count == 0) return 0;
  uint8_t data[2 + 9 * 2] = {0xB1, count};
  size_t n = 2;
  const auto appendId = [&n, &data](uint16_t id) {
    data[n++] = static_cast<uint8_t>(id & 0xFF);
    data[n++] = static_cast<uint8_t>(id >> 8);
  };
  if (queryIeco) appendId(kPropIeco);
  if (queryOutSilent) appendId(kPropOutSilent);
  if (querySelfClean) appendId(kPropSelfClean);
  if (queryBreezeAway) appendId(kPropBreezeAway);
  if (queryBreezeless) appendId(kPropBreezeless);
  if (queryFlash) appendId(kPropFlash);
  if (queryRateSelect) appendId(kPropRateSelect);
  if (queryCascade) appendId(kPropCascade);
  if (queryFreshAir) appendId(kPropFreshAir);
  return commandFrame(out, outCap, kFrameTypeQuery, data, n);
}

size_t SetPropertiesCommand::serialize(uint8_t* out, size_t outCap) const {
  const uint8_t count = static_cast<uint8_t>((setIeco ? 1 : 0) + (setOutSilent ? 1 : 0) + (setSelfClean ? 1 : 0) +
                                             (setBreezeAway ? 1 : 0) + (setBreezeless ? 1 : 0) + (setFlash ? 1 : 0) +
                                             (setRateSelect ? 1 : 0) + (setCascade ? 1 : 0) + (setFreshAir ? 1 : 0) +
                                             (setBuzzer ? 1 : 0));
  if (count == 0) return 0;
  uint8_t data[kPropertiesMaxDataLen] = {0xB0, count};
  size_t n = 2;
  const auto appendBool = [&n, &data](uint16_t id, bool on) {
    data[n++] = static_cast<uint8_t>(id & 0xFF);
    data[n++] = static_cast<uint8_t>(id >> 8);
    data[n++] = 0x01;
    data[n++] = on ? 0x01 : 0x00;
  };
  if (setIeco) {
    // PropertyId.IECO.encode: [0, ieco_number, ieco_switch] + 10 zero bytes.
    data[n++] = static_cast<uint8_t>(kPropIeco & 0xFF);
    data[n++] = static_cast<uint8_t>(kPropIeco >> 8);
    data[n++] = kIecoValueLen;
    data[n++] = 0x00;
    data[n++] = iecoNumber;
    data[n++] = iecoOn ? 0x01 : 0x00;
    for (size_t i = 0; i < 10; ++i)
      data[n++] = 0x00;
  }
  if (setOutSilent) {
    // PropertyId.OUT_SILENT.encode: [on ? 3 : 0].
    data[n++] = static_cast<uint8_t>(kPropOutSilent & 0xFF);
    data[n++] = static_cast<uint8_t>(kPropOutSilent >> 8);
    data[n++] = 0x01;
    data[n++] = outSilentOn ? 0x03 : 0x00;
  }
  if (setSelfClean) {
    // PropertyId.SELF_CLEAN.encode: [1]; the device only accepts the trigger
    // (msmart start_self_clean sends True and never off).
    data[n++] = static_cast<uint8_t>(kPropSelfClean & 0xFF);
    data[n++] = static_cast<uint8_t>(kPropSelfClean >> 8);
    data[n++] = 0x01;
    data[n++] = 0x01;
  }
  if (setBreezeAway) {
    // PropertyId.BREEZE_AWAY.encode: [on ? 2 : 1]; 1 is the explicit off value.
    data[n++] = static_cast<uint8_t>(kPropBreezeAway & 0xFF);
    data[n++] = static_cast<uint8_t>(kPropBreezeAway >> 8);
    data[n++] = 0x01;
    data[n++] = breezeAwayOn ? 0x02 : 0x01;
  }
  if (setBreezeless) {
    appendBool(kPropBreezeless, breezelessOn);
  }
  if (setFlash) {
    appendBool(kPropFlash, flashOn);
  }
  if (setRateSelect) {
    // RATE_SELECT.encode: the raw gear byte (100 = off).
    data[n++] = static_cast<uint8_t>(kPropRateSelect & 0xFF);
    data[n++] = static_cast<uint8_t>(kPropRateSelect >> 8);
    data[n++] = 0x01;
    data[n++] = rateSelectValue;
  }
  if (setCascade) {
    // CASCADE.encode: [mode != 0 ? 1 : 0, mode].
    data[n++] = static_cast<uint8_t>(kPropCascade & 0xFF);
    data[n++] = static_cast<uint8_t>(kPropCascade >> 8);
    data[n++] = 0x02;
    data[n++] = cascadeMode != 0 ? 0x01 : 0x00;
    data[n++] = cascadeMode;
  }
  if (setFreshAir) {
    // FRESH_AIR.encode: [speed != 0 ? 1 : 0, speed, 0xFF].
    data[n++] = static_cast<uint8_t>(kPropFreshAir & 0xFF);
    data[n++] = static_cast<uint8_t>(kPropFreshAir >> 8);
    data[n++] = 0x03;
    data[n++] = freshAirSpeed != 0 ? 0x01 : 0x00;
    data[n++] = freshAirSpeed;
    data[n++] = 0xFF;
  }
  if (setBuzzer) {
    // BUZZER.encode: bool as one byte. Not decodable back; msmart's device
    // layer keeps its own copy and re-appends this TLV to every set.
    appendBool(kPropBuzzer, buzzerOn);
  }
  return commandFrame(out, outCap, kFrameTypeControl, data, n);
}

} // namespace midea
