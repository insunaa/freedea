#include "Sha256.h"

namespace midea {
namespace {

// Round constants floor(frac(cbrt(prime_i)) * 2^32), generated from the
// defining formula; every entry is exercised by the KAT host tests.
constexpr uint32_t kSha256K[64] = {
    0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
    0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
    0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
    0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
    0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
    0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
    0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
    0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2,
};

constexpr uint32_t rotr32(uint32_t v, unsigned n) {
  return (v >> n) | (v << (32 - n));
}

constexpr uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (~x & z);
}
constexpr uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}
constexpr uint32_t bigSigma0(uint32_t x) {
  return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22);
}
constexpr uint32_t bigSigma1(uint32_t x) {
  return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25);
}
constexpr uint32_t smallSigma0(uint32_t x) {
  return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
}
constexpr uint32_t smallSigma1(uint32_t x) {
  return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10);
}

} // namespace

void Sha256::reset() {
  state_[0] = 0x6A09E667;
  state_[1] = 0xBB67AE85;
  state_[2] = 0x3C6EF372;
  state_[3] = 0xA54FF53A;
  state_[4] = 0x510E527F;
  state_[5] = 0x9B05688C;
  state_[6] = 0x1F83D9AB;
  state_[7] = 0x5BE0CD19;
  bitLen_ = 0;
  bufLen_ = 0;
}

void Sha256::update(const void* data, size_t len) {
  bitLen_ += static_cast<uint64_t>(len) * 8;
  feed(static_cast<const uint8_t*>(data), len);
}

void Sha256::final(uint8_t out[kSha256DigestSize]) {
  const uint64_t bits = bitLen_;
  static const uint8_t kPadStart = 0x80;
  static const uint8_t kZeros[63] = {};
  feed(&kPadStart, 1);
  // Zero-pad until exactly 56 bytes are buffered, leaving room for the length.
  feed(kZeros, (56 - bufLen_ + 64) % 64);
  uint8_t len8[8];
  for (size_t i = 0; i < 8; ++i)
    len8[i] = static_cast<uint8_t>(bits >> (8 * (7 - i))); // big-endian
  feed(len8, 8);                                           // fills the block to 64 and transforms it
  for (size_t i = 0; i < 8; ++i) {
    for (size_t j = 0; j < 4; ++j)
      out[i * 4 + j] = static_cast<uint8_t>(state_[i] >> (8 * (3 - j)));
  }
  reset();
}

void Sha256::feed(const uint8_t* data, size_t len) {
  if (bufLen_ != 0) {
    size_t n = 64 - bufLen_;
    if (len < n) n = len;
    for (size_t i = 0; i < n; ++i)
      buf_[bufLen_ + i] = data[i];
    bufLen_ += n;
    data += n;
    len -= n;
    if (bufLen_ == 64) {
      transform(buf_);
      bufLen_ = 0;
    }
  }
  while (len >= 64) {
    transform(data);
    data += 64;
    len -= 64;
  }
  for (size_t i = 0; i < len; ++i)
    buf_[bufLen_++] = data[i];
}

void Sha256::transform(const uint8_t* block) {
  // Rolling 16-word schedule keeps stack use small on the ESP32-C3.
  uint32_t w[16];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[4 * i]) << 24) | (static_cast<uint32_t>(block[4 * i + 1]) << 16) |
           (static_cast<uint32_t>(block[4 * i + 2]) << 8) | static_cast<uint32_t>(block[4 * i + 3]);
  }
  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
  for (int i = 0; i < 64; ++i) {
    if (i >= 16) {
      const int r = i & 15;
      w[r] += smallSigma1(w[(i - 2) & 15]) + smallSigma0(w[(i - 15) & 15]) + w[(i - 7) & 15];
    }
    const uint32_t t1 = h + bigSigma1(e) + ch(e, f, g) + kSha256K[i] + w[i & 15];
    const uint32_t t2 = bigSigma0(a) + maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

} // namespace midea
