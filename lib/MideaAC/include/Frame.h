#pragma once

// Midea frame build/validate: 10-byte header + payload + checksum byte.
// Port of msmart frame.py at d7db53b (Frame.tobytes/checksum/validate).

#include <cstddef>
#include <cstdint>

namespace midea {

constexpr size_t kFrameHeaderLen = 10;
constexpr uint8_t kFrameStart = 0xAA;
constexpr uint8_t kDeviceTypeAc = 0xAC;
constexpr uint8_t kFrameTypeControl = 0x02;
constexpr uint8_t kFrameTypeQuery = 0x03;

// Byte 1 of the header is header+data length, so the payload must fit the
// remaining room in that byte.
constexpr size_t kFrameMaxDataLen = 255 - kFrameHeaderLen;

// Two's-complement negation of the byte sum, as msmart Frame.checksum.
constexpr uint8_t frameChecksum(const uint8_t* data, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; ++i)
    sum = static_cast<uint8_t>(sum + data[i]);
  return static_cast<uint8_t>(~sum + 1);
}

// Writes header + data + checksum into out[0..outCap), mirroring
// Frame.tobytes (protocol version byte 8 is always 0, like Command).
// Returns total frame length, or 0 if data is too long or outCap too small.
inline size_t buildFrame(uint8_t* out, size_t outCap, uint8_t deviceType, uint8_t frameType, const uint8_t* data,
                         size_t dataLen) {
  if (out == nullptr || dataLen > kFrameMaxDataLen || outCap < kFrameHeaderLen + dataLen + 1) {
    return 0;
  }
  out[0] = kFrameStart;
  out[1] = static_cast<uint8_t>(dataLen + kFrameHeaderLen);
  out[2] = deviceType;
  for (size_t i = 3; i < 9; ++i)
    out[i] = 0; // unused bytes + protocol version
  out[9] = frameType;
  for (size_t i = 0; i < dataLen; ++i)
    out[kFrameHeaderLen + i] = data[i];
  const size_t len = kFrameHeaderLen + dataLen;
  out[len] = frameChecksum(out + 1, len - 1); // sum excludes start byte and checksum
  return len + 1;
}

// Mirrors Frame.validate: minimum length, then checksum over bytes
// [1, len-1) compared to the final byte, then device type at offset 2.
// Deliberately does not cross-check frame[1] against len, matching upstream.
inline bool validateFrame(const uint8_t* frame, size_t len, uint8_t expectedDeviceType) {
  if (frame == nullptr || len < kFrameHeaderLen) return false;
  if (frameChecksum(frame + 1, len - 2) != frame[len - 1]) return false;
  return frame[2] == expectedDeviceType;
}

} // namespace midea
