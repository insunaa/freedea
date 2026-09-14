#pragma once

// V2 LAN packet build/parse, port of msmart _Packet (lan.py 686-757) at
// d7db53b: 40-byte header + AES-128-ECB/PKCS7(frame) + md5 of everything
// preceding the hash.

#include <cstddef>
#include <cstdint>

#include "Md5.h"
#include "Security.h"

namespace midea {

inline constexpr size_t kV2PacketHeaderLen = 40;
inline constexpr size_t kV2PacketHashLen = kMd5DigestSize;
inline constexpr size_t kV2PacketTimestampLen = 8;

// Smallest wire-valid packet: header + one ciphertext block + hash. Declared
// lengths below this cannot carry a PKCS#7-padded frame.
inline constexpr size_t kV2PacketMinLen = kV2PacketHeaderLen + kAesBlockSize + kV2PacketHashLen;

// Wire size of the packet carrying a frameLen-byte frame.
constexpr size_t v2PacketSize(size_t frameLen) {
  return kV2PacketHeaderLen + aesPkcs7PaddedSize(frameLen) + kV2PacketHashLen;
}

// Formats UTC parts into the 8-byte packet timestamp (lan.py _timestamp).
// Byte order is hundredths-first: hundredths, second, minute, hour, day,
// month, year%100, year/100.
inline void buildV2Timestamp(uint8_t out[kV2PacketTimestampLen], uint16_t year, uint8_t month, uint8_t day,
                             uint8_t hour, uint8_t minute, uint8_t second, uint8_t hundredths) {
  out[0] = hundredths;
  out[1] = second;
  out[2] = minute;
  out[3] = hour;
  out[4] = day;
  out[5] = month;
  out[6] = static_cast<uint8_t>(year % 100);
  out[7] = static_cast<uint8_t>(year / 100);
}

// Encodes a frame into a V2 packet (lan.py _Packet.encode). The timestamp is
// caller-supplied (device time arrives via SNTP in a later phase; host tests
// fix it for byte-exactness). deviceId is written little-endian at [20:28]
// (only the low 6 bytes are meaningful upstream). Requires
// outCap >= v2PacketSize(frameLen); *outLen receives the packet size.
bool encodeV2Packet(const uint8_t* frame, size_t frameLen, uint64_t deviceId,
                    const uint8_t timestamp[kV2PacketTimestampLen], uint8_t* out, size_t outCap, size_t* outLen);

// Decodes a V2 packet (lan.py _Packet.decode): requires the 5A5A start, trims
// to the LE16 declared length at [4:6] (extra trailing bytes ignored),
// verifies the md5 tail, then AES-ECB-decrypts packet[40:len-16] and strips
// PKCS#7. The decrypted frame is not validated here (use validateFrame).
// *frameLen receives the frame size. frameOutCap must be at least the
// ciphertext size (a v2PacketSize-sized buffer is always sufficient).
bool decodeV2Packet(const uint8_t* data, size_t len, uint8_t* frameOut, size_t frameOutCap, size_t* frameLen);

} // namespace midea
