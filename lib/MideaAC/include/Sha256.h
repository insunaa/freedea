#pragma once

// Minimal streaming SHA-256 (FIPS 180-4) for the Midea LAN Security layer.
// Portable in-repo implementation compiled on both host and device;
// mbedtls stays reserved for AES only. Stack-sized context, no heap.

#include <cstddef>
#include <cstdint>

namespace midea {

constexpr size_t kSha256DigestSize = 32;

class Sha256 {
public:
  Sha256() { reset(); }

  void reset();
  void update(const void* data, size_t len);
  // Writes the 32-byte digest, then resets so the object can be reused.
  void final(uint8_t out[kSha256DigestSize]);

private:
  void feed(const uint8_t* data, size_t len);
  void transform(const uint8_t* block);

  uint32_t state_[8];
  uint64_t bitLen_;
  uint8_t buf_[64];
  size_t bufLen_;
};

inline void sha256(const void* data, size_t len, uint8_t out[kSha256DigestSize]) {
  Sha256 ctx;
  ctx.update(data, len);
  ctx.final(out);
}

} // namespace midea
