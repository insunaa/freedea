#pragma once

// Midea LAN Security primitives, port of msmart lan.py Security (650-684):
// MD5 signing, key derivation, udpid, and AES (2.2b). Hashes use the
// portable in-repo Md5/Sha256 on both host and device. AES binds the
// ESP-IDF mbedtls hardware port (esp_aes_*) on device — the prebuilt libs
// do not link the software mbedtls_aes_* entry points under
// CONFIG_MBEDTLS_HARDWARE_AES — and software mbedtls on the host.

#include <array>
#include <cstddef>
#include <cstdint>

#include "Md5.h"

namespace midea {

// Static signing key (lan.py:651), 36 ASCII bytes.
inline constexpr char kSignKey[] = "xhdiwjnchekd4d512chdjx5d8e4c394D2D7S";
inline constexpr size_t kSignKeyLen = sizeof(kSignKey) - 1;

// md5(SIGN_KEY) (lan.py:652). Host test verifies against the golden fixture.
inline constexpr std::array<uint8_t, 16> kEncKey = {
    0x6A, 0x92, 0xEF, 0x40, 0x6B, 0xAD, 0x2F, 0x03, 0x59, 0xBA, 0xAD, 0x99, 0x41, 0x71, 0xEA, 0x6D,
};

// md5(data || SIGN_KEY) (lan.py:677-678).
void sign(const uint8_t* data, size_t len, uint8_t out[kMd5DigestSize]);

// sha256(id)[:16] XOR sha256(id)[16:] (lan.py:680-683). Discovery tries both
// byte orders of the 6-byte device id.
void udpid(const uint8_t* deviceId, size_t len, uint8_t out[16]);

inline constexpr size_t kAesBlockSize = 16;

// PKCS#7 padded size for a len-byte message (pad is always 1..16 bytes, so a
// block-sized message grows by a full block).
constexpr size_t aesPkcs7PaddedSize(size_t len) {
  return len + kAesBlockSize - (len % kAesBlockSize);
}

// AES-128-ECB + PKCS#7 with kEncKey (lan.py encrypt_aes, 669-674). outCap
// must be >= aesPkcs7PaddedSize(len); *outLen receives the written size.
bool encryptAesPkcs7(const uint8_t* data, size_t len, uint8_t* out, size_t outCap, size_t* outLen);

// AES-128-ECB decrypt + PKCS#7 validate/strip (lan.py decrypt_aes, 662-667).
// Rejects non-block-multiple input and invalid padding. out may alias data.
bool decryptAesPkcs7(const uint8_t* data, size_t len, uint8_t* out, size_t outCap, size_t* outLen);

// AES-CBC with a 16-byte zero IV, no padding (lan.py encrypt/decrypt_aes_cbc,
// 654-660). len must be a multiple of kAesBlockSize. keyLen is 16 or 32
// bytes; 192-bit keys are rejected because the ESP32-C3 hardware AES lacks
// them. V3's local_key path is always 32. Partial overlap is undefined;
// full in-place (out == data) is supported (mbedtls guarantees it, and the
// V3 packet codec relies on it).
bool encryptAesCbc(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, uint8_t* out, size_t outCap);
bool decryptAesCbc(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, uint8_t* out, size_t outCap);

} // namespace midea
