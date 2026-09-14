#pragma once

// V3 LAN packet build/parse, port of msmart _LanProtocolV3 (lan.py 146-426):
// 6-byte header (8370, BE16 size, 0x20, pad<<4|type), 2-byte request id, and
// a payload AES-256-CBC-encrypted (zero IV) with the per-session local_key,
// plus a 32-byte SHA256 sign over header || cleartext payload. The size field
// counts payload + padding + sign but NOT the 2-byte id; wire total = size+8.
// Only the packet codec lives here; buffering, packet_id counting and the
// authenticate handshake belong to the transport (Phase 4.2).

#include <cstddef>
#include <cstdint>

#include "Security.h"
#include "Sha256.h"

namespace midea {

inline constexpr uint8_t kV3Start0 = 0x83;
inline constexpr uint8_t kV3Start1 = 0x70;
inline constexpr uint8_t kV3Magic = 0x20;
inline constexpr size_t kV3HeaderLen = 6;
inline constexpr size_t kV3PacketIdLen = 2;
inline constexpr size_t kV3SignLen = kSha256DigestSize;
inline constexpr size_t kV3LocalKeyLen = 32; // AES-256 session key

// PacketType nibble in header[5] (lan.py 151-156).
enum class V3PacketType : uint8_t {
  HandshakeRequest = 0x0,
  HandshakeResponse = 0x1,
  EncryptedResponse = 0x3,
  EncryptedRequest = 0x6,
  Error = 0xF,
};

// Pad byte count for 16-byte payload alignment (lan.py 331-332). The 2-byte
// request id counts in the alignment math but not in the size field.
constexpr size_t v3PadSize(size_t dataLen) {
  const size_t remainder = (dataLen + kV3PacketIdLen) % kAesBlockSize;
  return remainder == 0 ? 0 : kAesBlockSize - remainder;
}

// Wire size of an encrypted packet carrying dataLen bytes:
// header + id + padded payload + sign.
constexpr size_t v3EncryptedPacketSize(size_t dataLen) {
  return kV3HeaderLen + kV3PacketIdLen + dataLen + v3PadSize(dataLen) + kV3SignLen;
}

// Wire size of a handshake request carrying tokenLen bytes.
constexpr size_t v3HandshakePacketSize(size_t tokenLen) {
  return kV3HeaderLen + kV3PacketIdLen + tokenLen;
}

// Type nibble of a received packet; caller guarantees at least kV3HeaderLen
// bytes with the 8370 start.
constexpr V3PacketType v3PacketType(const uint8_t* header) {
  return static_cast<V3PacketType>(header[5] & 0x0F);
}

// Encrypted request (lan.py _encode_encrypted_request, 324-347). padBytes
// must hold exactly v3PadSize(dataLen) bytes (device: CSPRNG; host tests:
// fixture, since Python draws random pad). Requires
// outCap >= v3EncryptedPacketSize(dataLen).
bool encodeV3EncryptedRequest(const uint8_t localKey[kV3LocalKeyLen], uint16_t packetId, const uint8_t* data,
                              size_t dataLen, const uint8_t* padBytes, uint8_t* out, size_t outCap, size_t* outLen);

// Handshake request (lan.py _encode_handshake_request, 349-359): unencrypted,
// no sign; the size field carries only the token length.
bool encodeV3HandshakeRequest(uint16_t packetId, const uint8_t* token, size_t tokenLen, uint8_t* out, size_t outCap,
                              size_t* outLen);

// Decrypts and verifies one encrypted packet (lan.py _decode_encrypted_response,
// 245-273): AES-256-CBC-decrypt, verify sha256(header || cleartext) against
// the 32-byte tail, then strip the 2-byte id and the header[5]>>4 pad bytes.
// No type dispatch, so it also accepts encoder output (type 6) for round-trip
// tests, mirroring how msmart tests call the private handler directly.
bool decodeV3EncryptedResponse(const uint8_t localKey[kV3LocalKeyLen], const uint8_t* packet, size_t len,
                               uint8_t* payloadOut, size_t payloadOutCap, size_t* payloadOutLen);

// Processes one received packet (lan.py _process_packet 284-304 plus the
// response handlers). The full packet spans the BE16 size field + 8; trailing
// bytes beyond it are ignored. Encrypted responses (type 3) go through
// decodeV3EncryptedResponse; handshake responses (type 1) return packet[8:]
// raw and need no key (pass nullptr). Request and error types fail.
bool decodeV3Packet(const uint8_t* localKey, const uint8_t* packet, size_t len, uint8_t* payloadOut,
                    size_t payloadOutCap, size_t* payloadOutLen);

// Derives the session key from a 64-byte handshake response payload (lan.py
// _get_local_key, 379-397): AES-256-CBC-decrypt(key, data[:32]), verify its
// sha256 equals data[32:64], then local_key = decrypted XOR key.
bool deriveV3LocalKey(const uint8_t key[kV3LocalKeyLen], const uint8_t* handshakeData,
                      uint8_t localKeyOut[kV3LocalKeyLen]);

} // namespace midea
