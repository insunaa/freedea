#include "V3Packet.h"

#include <cstring>

namespace midea {

bool encodeV3EncryptedRequest(const uint8_t localKey[kV3LocalKeyLen], uint16_t packetId, const uint8_t* data,
                              size_t dataLen, const uint8_t* padBytes, uint8_t* out, size_t outCap, size_t* outLen) {
  if (localKey == nullptr || data == nullptr || out == nullptr || outLen == nullptr) {
    return false;
  }
  const size_t pad = v3PadSize(dataLen);
  if (pad > 0 && padBytes == nullptr) {
    return false;
  }
  const size_t total = v3EncryptedPacketSize(dataLen);
  if (outCap < total || total > 0xFFFFu) {
    return false;
  }

  const size_t payloadLen = kV3PacketIdLen + dataLen + pad;
  const uint16_t sizeField = static_cast<uint16_t>(dataLen + pad + kV3SignLen);
  out[0] = kV3Start0;
  out[1] = kV3Start1;
  out[2] = static_cast<uint8_t>(sizeField >> 8);
  out[3] = static_cast<uint8_t>(sizeField & 0xFF);
  out[4] = kV3Magic;
  out[5] = static_cast<uint8_t>((pad << 4) | static_cast<uint8_t>(V3PacketType::EncryptedRequest));

  uint8_t* payload = out + kV3HeaderLen;
  payload[0] = static_cast<uint8_t>(packetId >> 8);
  payload[1] = static_cast<uint8_t>(packetId & 0xFF);
  std::memcpy(payload + kV3PacketIdLen, data, dataLen);
  if (pad > 0) {
    std::memcpy(payload + kV3PacketIdLen + dataLen, padBytes, pad);
  }

  // Sign over header || cleartext payload (contiguous in out before
  // encryption), then encrypt the payload region in place.
  uint8_t digest[kSha256DigestSize];
  sha256(out, kV3HeaderLen + payloadLen, digest);
  std::memcpy(out + kV3HeaderLen + payloadLen, digest, kSha256DigestSize);

  if (!encryptAesCbc(localKey, kV3LocalKeyLen, payload, payloadLen, payload, payloadLen)) {
    return false;
  }
  *outLen = total;
  return true;
}

bool encodeV3HandshakeRequest(uint16_t packetId, const uint8_t* token, size_t tokenLen, uint8_t* out, size_t outCap,
                              size_t* outLen) {
  if (token == nullptr || out == nullptr || outLen == nullptr) {
    return false;
  }
  const size_t total = v3HandshakePacketSize(tokenLen);
  if (outCap < total || tokenLen > 0xFFFFu) {
    return false;
  }
  out[0] = kV3Start0;
  out[1] = kV3Start1;
  // Size field counts only the token, not the 2-byte id (lan.py 353-354).
  out[2] = static_cast<uint8_t>(tokenLen >> 8);
  out[3] = static_cast<uint8_t>(tokenLen & 0xFF);
  out[4] = kV3Magic;
  out[5] = static_cast<uint8_t>(V3PacketType::HandshakeRequest);
  out[6] = static_cast<uint8_t>(packetId >> 8);
  out[7] = static_cast<uint8_t>(packetId & 0xFF);
  std::memcpy(out + kV3HeaderLen + kV3PacketIdLen, token, tokenLen);
  *outLen = total;
  return true;
}

bool decodeV3EncryptedResponse(const uint8_t localKey[kV3LocalKeyLen], const uint8_t* packet, size_t len,
                               uint8_t* payloadOut, size_t payloadOutCap, size_t* payloadOutLen) {
  if (packet == nullptr || payloadOut == nullptr || payloadOutLen == nullptr || len < kV3HeaderLen) {
    return false;
  }
  if (packet[0] != kV3Start0 || packet[1] != kV3Start1 || packet[4] != kV3Magic) {
    return false;
  }
  const size_t total = (static_cast<size_t>(packet[2]) << 8 | packet[3]) + 8;
  if (len < total || total < kV3HeaderLen + kV3SignLen) {
    return false;
  }
  const size_t cipherLen = total - kV3HeaderLen - kV3SignLen;
  if (cipherLen == 0 || cipherLen % kAesBlockSize != 0 || payloadOutCap < cipherLen) {
    return false;
  }
  std::memcpy(payloadOut, packet + kV3HeaderLen, cipherLen);
  if (!decryptAesCbc(localKey, kV3LocalKeyLen, payloadOut, cipherLen, payloadOut, cipherLen)) {
    return false;
  }
  Sha256 ctx;
  ctx.update(packet, kV3HeaderLen);
  ctx.update(payloadOut, cipherLen);
  uint8_t digest[kSha256DigestSize];
  ctx.final(digest);
  if (std::memcmp(digest, packet + kV3HeaderLen + cipherLen, kV3SignLen) != 0) {
    return false;
  }
  // Cleartext payload = 2-byte id + data + pad. A pad nibble of 0 slices to
  // the end here; upstream's payload[2:-pad] empty-slice quirk drops the
  // payload whole, but real payloads (V2 packets) always pad to 6.
  const size_t pad = packet[5] >> 4;
  if (cipherLen < kV3PacketIdLen + pad) {
    return false;
  }
  const size_t outLen = cipherLen - kV3PacketIdLen - pad;
  std::memmove(payloadOut, payloadOut + kV3PacketIdLen, outLen);
  *payloadOutLen = outLen;
  return true;
}

bool decodeV3Packet(const uint8_t* localKey, const uint8_t* packet, size_t len, uint8_t* payloadOut,
                    size_t payloadOutCap, size_t* payloadOutLen) {
  if (packet == nullptr || payloadOut == nullptr || payloadOutLen == nullptr) {
    return false;
  }
  if (len < kV3HeaderLen || packet[0] != kV3Start0 || packet[1] != kV3Start1) {
    return false;
  }
  if (packet[4] != kV3Magic) {
    return false;
  }
  const size_t total = (static_cast<size_t>(packet[2]) << 8 | packet[3]) + 8;
  if (len < total) {
    return false;
  }

  switch (v3PacketType(packet)) {
    case V3PacketType::EncryptedResponse:
      return localKey != nullptr &&
             decodeV3EncryptedResponse(localKey, packet, len, payloadOut, payloadOutCap, payloadOutLen);
    case V3PacketType::HandshakeResponse: {
      if (total < kV3HeaderLen + kV3PacketIdLen) {
        return false;
      }
      const size_t outLen = total - kV3HeaderLen - kV3PacketIdLen;
      if (payloadOutCap < outLen) {
        return false;
      }
      std::memcpy(payloadOut, packet + kV3HeaderLen + kV3PacketIdLen, outLen);
      *payloadOutLen = outLen;
      return true;
    }
    default:
      // Requests and error packets are never received (_process_packet raises).
      return false;
  }
}

bool deriveV3LocalKey(const uint8_t key[kV3LocalKeyLen], const uint8_t* handshakeData,
                      uint8_t localKeyOut[kV3LocalKeyLen]) {
  if (key == nullptr || handshakeData == nullptr || localKeyOut == nullptr) {
    return false;
  }
  if (!decryptAesCbc(key, kV3LocalKeyLen, handshakeData, kV3LocalKeyLen, localKeyOut, kV3LocalKeyLen)) {
    return false;
  }
  uint8_t digest[kSha256DigestSize];
  sha256(localKeyOut, kV3LocalKeyLen, digest);
  if (std::memcmp(digest, handshakeData + kV3LocalKeyLen, kV3SignLen) != 0) {
    return false;
  }
  // strxor(decrypted, key) (lan.py 396).
  for (size_t i = 0; i < kV3LocalKeyLen; ++i) {
    localKeyOut[i] ^= key[i];
  }
  return true;
}

} // namespace midea
