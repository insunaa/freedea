#include "Md5.h"

namespace midea {
namespace {

// Round constants floor(abs(sin(i + 1)) * 2^32), generated from the defining
// formula; every entry is exercised by the KAT host tests.
constexpr uint32_t kMd5K[64] = {
    0xD76AA478, 0xE8C7B756, 0x242070DB, 0xC1BDCEEE, 0xF57C0FAF, 0x4787C62A, 0xA8304613, 0xFD469501,
    0x698098D8, 0x8B44F7AF, 0xFFFF5BB1, 0x895CD7BE, 0x6B901122, 0xFD987193, 0xA679438E, 0x49B40821,
    0xF61E2562, 0xC040B340, 0x265E5A51, 0xE9B6C7AA, 0xD62F105D, 0x02441453, 0xD8A1E681, 0xE7D3FBC8,
    0x21E1CDE6, 0xC33707D6, 0xF4D50D87, 0x455A14ED, 0xA9E3E905, 0xFCEFA3F8, 0x676F02D9, 0x8D2A4C8A,
    0xFFFA3942, 0x8771F681, 0x6D9D6122, 0xFDE5380C, 0xA4BEEA44, 0x4BDECFA9, 0xF6BB4B60, 0xBEBFBC70,
    0x289B7EC6, 0xEAA127FA, 0xD4EF3085, 0x04881D05, 0xD9D4D039, 0xE6DB99E5, 0x1FA27CF8, 0xC4AC5665,
    0xF4292244, 0x432AFF97, 0xAB9423A7, 0xFC93A039, 0x655B59C3, 0x8F0CCC92, 0xFFEFF47D, 0x85845DD1,
    0x6FA87E4F, 0xFE2CE6E0, 0xA3014314, 0x4E0811A1, 0xF7537E82, 0xBD3AF235, 0x2AD7D2BB, 0xEB86D391,
};

// Shift amounts per round group, row-major: round r = i >> 4 uses the 4
// entries starting at kMd5Shifts[4 * r], cycled by (i & 3) within the round.
constexpr uint8_t kMd5Shifts[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};

constexpr uint32_t rotl32(uint32_t v, unsigned n) {
  return (v << n) | (v >> (32 - n));
}

} // namespace

void Md5::reset() {
  state_[0] = 0x67452301;
  state_[1] = 0xEFCDAB89;
  state_[2] = 0x98BADCFE;
  state_[3] = 0x10325476;
  bitLen_ = 0;
  bufLen_ = 0;
}

void Md5::update(const void* data, size_t len) {
  bitLen_ += static_cast<uint64_t>(len) * 8;
  feed(static_cast<const uint8_t*>(data), len);
}

void Md5::final(uint8_t out[kMd5DigestSize]) {
  const uint64_t bits = bitLen_;
  static const uint8_t kPadStart = 0x80;
  static const uint8_t kZeros[63] = {};
  feed(&kPadStart, 1);
  // Zero-pad until exactly 56 bytes are buffered, leaving room for the length.
  feed(kZeros, (56 - bufLen_ + 64) % 64);
  uint8_t len8[8];
  for (size_t i = 0; i < 8; ++i)
    len8[i] = static_cast<uint8_t>(bits >> (8 * i));
  feed(len8, 8); // fills the block to 64 and transforms it
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j)
      out[i * 4 + j] = static_cast<uint8_t>(state_[i] >> (8 * j));
  }
  reset();
}

void Md5::feed(const uint8_t* data, size_t len) {
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

void Md5::transform(const uint8_t* block) {
  uint32_t x[16];
  for (int i = 0; i < 16; ++i) {
    x[i] = static_cast<uint32_t>(block[4 * i]) | (static_cast<uint32_t>(block[4 * i + 1]) << 8) |
           (static_cast<uint32_t>(block[4 * i + 2]) << 16) | (static_cast<uint32_t>(block[4 * i + 3]) << 24);
  }
  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  for (uint32_t i = 0; i < 64; ++i) {
    uint32_t f;
    uint32_t g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) & 15;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) & 15;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) & 15;
    }
    const uint32_t tmp = d;
    d = c;
    c = b;
    b += rotl32(a + f + kMd5K[i] + x[g], kMd5Shifts[((i >> 4) << 2) | (i & 3)]);
    a = tmp;
  }
  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
}

} // namespace midea
