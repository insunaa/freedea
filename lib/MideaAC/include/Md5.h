#pragma once

// Minimal streaming MD5 (RFC 1321) for the Midea LAN Security layer.
// Portable in-repo implementation compiled on both host and device;
// mbedtls stays reserved for AES only. Stack-sized context, no heap.

#include <cstddef>
#include <cstdint>

namespace midea {

constexpr size_t kMd5DigestSize = 16;

class Md5 {
public:
  Md5() { reset(); }

  void reset();
  void update(const void* data, size_t len);
  // Writes the 16-byte digest, then resets so the object can be reused.
  void final(uint8_t out[kMd5DigestSize]);

private:
  void feed(const uint8_t* data, size_t len);
  void transform(const uint8_t* block);

  uint32_t state_[4];
  uint64_t bitLen_;
  uint8_t buf_[64];
  size_t bufLen_;
};

inline void md5(const void* data, size_t len, uint8_t out[kMd5DigestSize]) {
  Md5 ctx;
  ctx.update(data, len);
  ctx.final(out);
}

} // namespace midea
