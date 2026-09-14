#include "Security.h"

#include <cstring>

#include "Sha256.h"

#if defined(ESP_PLATFORM)
// ESP-IDF mbedtls AES port: hardware-accelerated on ESP32-C3. The prebuilt
// libs do not export the software mbedtls_aes_* entry points when
// CONFIG_MBEDTLS_HARDWARE_AES is set, so the esp_aes_* API is the linkable
// mbedtls AES surface here.
#include "aes/esp_aes.h"
#else
#include <mbedtls/aes.h>
#endif

namespace {

// AES backend shim: esp_aes_* (device) / mbedtls_aes_* (host). Single
// implementation of the portable Security layer on top.

#if defined(ESP_PLATFORM)

using AesContext = esp_aes_context;

void aesInit(AesContext& ctx) {
  esp_aes_init(&ctx);
}
void aesFree(AesContext& ctx) {
  esp_aes_free(&ctx);
}

// The hardware port keeps one key schedule usable for both directions.
bool aesSetKey(AesContext& ctx, const uint8_t* key, size_t keyLen, bool /*encrypt*/) {
  return esp_aes_setkey(&ctx, key, static_cast<unsigned>(keyLen) * 8U) == 0;
}

bool aesEcbBlock(AesContext& ctx, bool encrypt, const uint8_t* in, uint8_t* out) {
  return esp_aes_crypt_ecb(&ctx, encrypt ? ESP_AES_ENCRYPT : ESP_AES_DECRYPT, in, out) == 0;
}

bool aesCbc(AesContext& ctx, bool encrypt, size_t len, uint8_t iv[16], const uint8_t* in, uint8_t* out) {
  return esp_aes_crypt_cbc(&ctx, encrypt ? ESP_AES_ENCRYPT : ESP_AES_DECRYPT, len, iv, in, out) == 0;
}

#else // Host: software mbedtls

using AesContext = mbedtls_aes_context;

void aesInit(AesContext& ctx) {
  mbedtls_aes_init(&ctx);
}
void aesFree(AesContext& ctx) {
  mbedtls_aes_free(&ctx);
}

bool aesSetKey(AesContext& ctx, const uint8_t* key, size_t keyLen, bool encrypt) {
  const unsigned keyBits = static_cast<unsigned>(keyLen) * 8U;
  const int rc = encrypt ? mbedtls_aes_setkey_enc(&ctx, key, keyBits) : mbedtls_aes_setkey_dec(&ctx, key, keyBits);
  return rc == 0;
}

bool aesEcbBlock(AesContext& ctx, bool encrypt, const uint8_t* in, uint8_t* out) {
  return mbedtls_aes_crypt_ecb(&ctx, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, in, out) == 0;
}

bool aesCbc(AesContext& ctx, bool encrypt, size_t len, uint8_t iv[16], const uint8_t* in, uint8_t* out) {
  return mbedtls_aes_crypt_cbc(&ctx, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, len, iv, in, out) == 0;
}

#endif

// RAII so key material is cleared on every return path. Device context is
// 34 bytes; the host context is only large on the host, where stack is
// plentiful.
class AesCtx {
public:
  AesCtx() { aesInit(ctx_); }
  ~AesCtx() { aesFree(ctx_); }
  AesCtx(const AesCtx&) = delete;
  AesCtx& operator=(const AesCtx&) = delete;
  AesContext& get() { return ctx_; }

private:
  AesContext ctx_;
};

bool aesKeyLenOk(size_t keyLen) {
  return keyLen == 16 || keyLen == 32;
}

// Encrypts len bytes (multiple of the block size) block by block. Handles
// out == data (in-place): each block read completes before its write.
bool ecbBlocks(AesCtx& ctx, bool encrypt, const uint8_t* in, uint8_t* out, size_t len) {
  for (size_t off = 0; off < len; off += midea::kAesBlockSize) {
    if (!aesEcbBlock(ctx.get(), encrypt, in + off, out + off)) return false;
  }
  return true;
}

} // namespace

namespace midea {

void sign(const uint8_t* data, size_t len, uint8_t out[kMd5DigestSize]) {
  Md5 ctx;
  ctx.update(data, len);
  ctx.update(kSignKey, kSignKeyLen);
  ctx.final(out);
}

void udpid(const uint8_t* deviceId, size_t len, uint8_t out[16]) {
  uint8_t hash[kSha256DigestSize];
  sha256(deviceId, len, hash);
  for (size_t i = 0; i < 16; ++i)
    out[i] = hash[i] ^ hash[16 + i];
}

bool encryptAesPkcs7(const uint8_t* data, size_t len, uint8_t* out, size_t outCap, size_t* outLen) {
  if (!data || !out || !outLen) return false;
  const size_t pad = kAesBlockSize - (len % kAesBlockSize);
  const size_t total = len + pad;
  if (outCap < total) return false;
  if (out != data) std::memcpy(out, data, len);
  std::memset(out + len, static_cast<int>(pad), pad);

  AesCtx ctx;
  if (!aesSetKey(ctx.get(), kEncKey.data(), kEncKey.size(), true)) return false;
  if (!ecbBlocks(ctx, true, out, out, total)) return false;
  *outLen = total;
  return true;
}

bool decryptAesPkcs7(const uint8_t* data, size_t len, uint8_t* out, size_t outCap, size_t* outLen) {
  if (!data || !out || !outLen) return false;
  if (len == 0 || len % kAesBlockSize != 0 || outCap < len) return false;

  AesCtx ctx;
  if (!aesSetKey(ctx.get(), kEncKey.data(), kEncKey.size(), false)) return false;
  if (!ecbBlocks(ctx, false, data, out, len)) return false;

  // PKCS#7 validate/strip, matching pycryptodome's Padding.unpad: pad value
  // must be 1..16 and repeat pad times across the last block.
  const uint8_t pad = out[len - 1];
  if (pad == 0 || pad > kAesBlockSize || pad > len) return false;
  for (size_t i = len - pad; i < len; ++i) {
    if (out[i] != pad) return false;
  }
  *outLen = len - pad;
  return true;
}

bool encryptAesCbc(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, uint8_t* out, size_t outCap) {
  if (!key || !data || !out || !aesKeyLenOk(keyLen)) return false;
  if (len % kAesBlockSize != 0 || outCap < len) return false;

  uint8_t iv[kAesBlockSize] = {0};
  AesCtx ctx;
  if (!aesSetKey(ctx.get(), key, keyLen, true)) return false;
  return len == 0 || aesCbc(ctx.get(), true, len, iv, data, out);
}

bool decryptAesCbc(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, uint8_t* out, size_t outCap) {
  if (!key || !data || !out || !aesKeyLenOk(keyLen)) return false;
  if (len % kAesBlockSize != 0 || outCap < len) return false;

  uint8_t iv[kAesBlockSize] = {0};
  AesCtx ctx;
  if (!aesSetKey(ctx.get(), key, keyLen, false)) return false;
  return len == 0 || aesCbc(ctx.get(), false, len, iv, data, out);
}

} // namespace midea
