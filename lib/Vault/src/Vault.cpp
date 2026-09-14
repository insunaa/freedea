#include "Vault.h"

#include <cstring>

#include "Sha256.h"

// AES backend shim mirroring lib/MideaAC Security.cpp: the ESP-IDF mbedtls
// hardware port on device, software mbedtls on host. Unlike the MideaAC CBC
// wrapper this one takes an explicit IV (the envelope's per-write nonce).
#ifdef ESP_PLATFORM
#include "aes/esp_aes.h"

namespace {
using AesContext = esp_aes_context;

void aesInit(AesContext* ctx) {
  esp_aes_init(ctx);
}
void aesFree(AesContext* ctx) {
  esp_aes_free(ctx);
}
// The hardware port keeps one key schedule usable in both directions.
bool aesSetKey256(AesContext* ctx, const uint8_t* key, bool /*encrypt*/) {
  return esp_aes_setkey(ctx, key, 256) == 0;
}
bool aesCbcBlock(AesContext* ctx, bool encrypt, uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len) {
  return esp_aes_crypt_cbc(ctx, encrypt ? ESP_AES_ENCRYPT : ESP_AES_DECRYPT, len, iv, in, out) == 0;
}
} // namespace

#else
#include <mbedtls/aes.h>

namespace {
using AesContext = mbedtls_aes_context;

void aesInit(AesContext* ctx) {
  mbedtls_aes_init(ctx);
}
void aesFree(AesContext* ctx) {
  mbedtls_aes_free(ctx);
}
// mbedtls shares one round-key buffer between the two schedules: calling
// both leaves only the last usable, so the schedule must match the direction.
bool aesSetKey256(AesContext* ctx, const uint8_t* key, bool encrypt) {
  const int rc = encrypt ? mbedtls_aes_setkey_enc(ctx, key, 256) : mbedtls_aes_setkey_dec(ctx, key, 256);
  return rc == 0;
}
bool aesCbcBlock(AesContext* ctx, bool encrypt, uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t len) {
  return mbedtls_aes_crypt_cbc(ctx, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, len, iv, in, out) == 0;
}
} // namespace

#endif

namespace vault {

namespace {

constexpr uint8_t kMagic[4] = {'F', 'D', 'V', '1'};
constexpr char kKeyDomain[] = "freedea:vault:v1";

// CBC with an explicit IV over len bytes (multiple of 16); in == out is
// supported by both backends (the IV bookkeeping uses saved values).
bool aesCbc(const uint8_t key[kKeyLen], bool encrypt, const uint8_t iv[kIvLen], const uint8_t* in, uint8_t* out,
            size_t len) {
  uint8_t ivBuf[kIvLen];
  std::memcpy(ivBuf, iv, kIvLen); // Both backends update the IV in place.
  AesContext ctx;
  aesInit(&ctx);
  bool ok = aesSetKey256(&ctx, key, encrypt);
  if (ok && len) ok = aesCbcBlock(&ctx, encrypt, ivBuf, in, out, len);
  aesFree(&ctx);
  return ok;
}

void putU16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void putU32(uint8_t* p, uint32_t v) {
  for (size_t i = 0; i < 4; ++i)
    p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint16_t getU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

void deriveKey(const uint8_t mac[kMacLen], uint8_t keyOut[kKeyLen]) {
  midea::Sha256 ctx;
  ctx.update(kKeyDomain, sizeof(kKeyDomain) - 1);
  ctx.update(mac, kMacLen);
  ctx.final(keyOut);
}

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
      crc = (crc >> 1) ^ mask;
    }
  }
  return ~crc;
}

size_t seal(Kind kind, const uint8_t key[kKeyLen], const uint8_t iv[kIvLen], const uint8_t* plain, size_t plainLen,
            uint8_t* out, size_t outCap) {
  if (!key || !iv || !out || (!plain && plainLen) || plainLen > UINT16_MAX) return 0;

  const size_t padded = paddedSize(plainLen);
  const size_t total = kHeaderLen + padded;
  if (outCap < total) return 0;

  std::memcpy(out, kMagic, sizeof(kMagic));
  out[4] = static_cast<uint8_t>(kind);
  out[5] = kFormatVersion;
  out[6] = kKeySrcMac;
  out[7] = 0;
  putU16(out + 8, static_cast<uint16_t>(plainLen));
  putU16(out + 10, 0);
  std::memcpy(out + 16, iv, kIvLen);
  std::memset(out + kHeaderLen + plainLen, 0, padded - plainLen);

  if (plainLen) std::memcpy(out + kHeaderLen, plain, plainLen);
  if (padded && !aesCbc(key, true, iv, out + kHeaderLen, out + kHeaderLen, padded)) return 0;

  // CRC covers iv||ciphertext (header bytes [16, end)), checked before any
  // decryption attempt.
  putU32(out + 12, crc32(out + 16, kIvLen + padded));
  return total;
}

OpenError open(Kind kind, const uint8_t key[kKeyLen], const uint8_t* in, size_t inLen, uint8_t* plainOut, size_t outCap,
               size_t* plainLenOut) {
  if (!key || !in || !plainOut || !plainLenOut) return OpenError::kInternal;
  if (inLen < kHeaderLen) return OpenError::kTooShort;
  if (std::memcmp(in, kMagic, sizeof(kMagic)) != 0) return OpenError::kBadMagic;
  if (in[5] != kFormatVersion) return OpenError::kBadFormat;
  if (in[4] != static_cast<uint8_t>(kind)) return OpenError::kBadKind;

  const size_t padded = inLen - kHeaderLen;
  if (padded & 0x0F) return OpenError::kCorrupt;
  const uint16_t payloadLen = getU16(in + 8);
  if (payloadLen > padded || payloadLen > outCap) return OpenError::kCorrupt;
  if (getU32(in + 12) != crc32(in + 16, kIvLen + padded)) return OpenError::kCorrupt;

  if (padded && !aesCbc(key, false, in + 16, in + kHeaderLen, plainOut, padded)) return OpenError::kInternal;
  *plainLenOut = payloadLen;
  return OpenError::kNone;
}

} // namespace vault
