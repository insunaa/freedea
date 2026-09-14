#include "Packet.h"

#include <cstring>

#include "Md5.h"
#include "Security.h"

namespace midea {
namespace {

// Fixed header field values (lan.py encode).
constexpr uint8_t kV2Start0 = 0x5A;
constexpr uint8_t kV2Start1 = 0x5A;
constexpr uint8_t kV2MsgTypeLo = 0x01; // message type LE16 0x1101
constexpr uint8_t kV2MsgTypeHi = 0x11;
constexpr uint8_t kV2Magic0 = 0x20;
constexpr uint8_t kV2Magic1 = 0x00;
constexpr size_t kV2MsgIdLen = 4;    // always zero on the wire
constexpr size_t kV2DeviceIdLen = 8; // LE, low 6 bytes meaningful
constexpr size_t kV2ReservedLen = 12;

static_assert(8 + kV2MsgIdLen + kV2PacketTimestampLen + kV2DeviceIdLen + kV2ReservedLen == kV2PacketHeaderLen,
              "V2 header layout must total 40 bytes");

} // namespace

bool encodeV2Packet(const uint8_t* frame, size_t frameLen, uint64_t deviceId,
                    const uint8_t timestamp[kV2PacketTimestampLen], uint8_t* out, size_t outCap, size_t* outLen) {
  if (frame == nullptr || timestamp == nullptr || out == nullptr || outLen == nullptr) {
    return false;
  }
  const size_t encLen = aesPkcs7PaddedSize(frameLen);
  const size_t total = kV2PacketHeaderLen + encLen + kV2PacketHashLen;
  if (total > 0xFFFF || outCap < total) {
    return false;
  }

  size_t n = 0;
  out[n++] = kV2Start0;
  out[n++] = kV2Start1;
  out[n++] = kV2MsgTypeLo;
  out[n++] = kV2MsgTypeHi;
  out[n++] = static_cast<uint8_t>(total & 0xFF);
  out[n++] = static_cast<uint8_t>(total >> 8);
  out[n++] = kV2Magic0;
  out[n++] = kV2Magic1;
  for (size_t i = 0; i < kV2MsgIdLen; ++i) {
    out[n++] = 0;
  }
  for (size_t i = 0; i < kV2PacketTimestampLen; ++i) {
    out[n++] = timestamp[i];
  }
  for (size_t i = 0; i < kV2DeviceIdLen; ++i) {
    out[n++] = static_cast<uint8_t>((deviceId >> (8 * i)) & 0xFF);
  }
  for (size_t i = 0; i < kV2ReservedLen; ++i) {
    out[n++] = 0;
  }

  size_t written = 0;
  if (!encryptAesPkcs7(frame, frameLen, out + kV2PacketHeaderLen, encLen, &written) || written != encLen) {
    return false;
  }
  n += encLen;
  sign(out, n, out + n); // md5 over header + ciphertext
  *outLen = n + kV2PacketHashLen;
  return true;
}

bool decodeV2Packet(const uint8_t* data, size_t len, uint8_t* frameOut, size_t frameOutCap, size_t* frameLen) {
  if (data == nullptr || frameOut == nullptr || frameLen == nullptr) {
    return false;
  }
  if (len < 6 || data[0] != kV2Start0 || data[1] != kV2Start1) {
    return false;
  }
  const size_t declared = static_cast<size_t>(data[4]) | (static_cast<size_t>(data[5]) << 8);
  if (declared < kV2PacketMinLen || len < declared) {
    return false;
  }

  uint8_t digest[kMd5DigestSize];
  sign(data, declared - kV2PacketHashLen, digest);
  if (std::memcmp(digest, data + declared - kV2PacketHashLen, kMd5DigestSize) != 0) {
    return false;
  }

  return decryptAesPkcs7(data + kV2PacketHeaderLen, declared - kV2PacketHeaderLen - kV2PacketHashLen, frameOut,
                         frameOutCap, frameLen);
}

} // namespace midea
