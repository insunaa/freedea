// Host test for the portable Md5/Sha256 and the midea Security layer
// (sign/udpid/keys/AES): RFC/FIPS known-answer tests with padding-boundary
// lengths (digests generated with Python hashlib) plus golden vectors from
// msmart's Security class (see vectors/NOTES.md). AES runs on the host
// software-mbedtls backend; the device uses the ESP-IDF HW port.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <mbedtls/aes.h>

#include "Md5.h"
#include "Security.h"
#include "Sha256.h"

namespace {

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool parseHex(const std::string& hex, std::vector<uint8_t>& out) {
  if (hex.size() % 2 != 0) return false;
  out.clear();
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int hi = hexNibble(hex[i]);
    const int lo = hexNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

bool hexEq(const uint8_t* digest, size_t len, const std::string& hex) {
  std::vector<uint8_t> expected;
  return parseHex(hex, expected) && expected.size() == len && std::memcmp(digest, expected.data(), len) == 0;
}

unsigned gFailures = 0;

void check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++gFailures;
}

using Fields = std::unordered_map<std::string, std::string>;

// Parses the shared vector format; supports empty values ("input" alone).
std::vector<std::pair<std::string, Fields>> loadVectors(const char* path) {
  std::vector<std::pair<std::string, Fields>> vectors;
  std::ifstream file(path);
  if (!file) {
    std::printf("FAIL: cannot open %s (run from test/host dir)\n", path);
    return vectors;
  }
  std::string line, name;
  Fields fields;
  bool have = false;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (line[0] != ' ') {
      if (have) vectors.emplace_back(name, fields);
      name = line;
      fields.clear();
      have = true;
      continue;
    }
    std::istringstream kv(line);
    std::string key, value;
    if (kv >> key) {
      while (kv.peek() == ' ' || kv.peek() == '\t')
        kv.get();
      std::getline(kv, value);
      // strip trailing whitespace
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
      fields[key] = value;
    }
  }
  if (have) vectors.emplace_back(name, fields);
  return vectors;
}

void fillPattern(uint8_t* buf, size_t n) {
  for (size_t i = 0; i < n; ++i)
    buf[i] = static_cast<uint8_t>((i * 7 + 3) & 0xFF);
}

// Host-test-only: raw AES-128-ECB with kEncKey (same software mbedtls the
// Security layer uses) to craft ciphertexts the API itself cannot emit,
// e.g. invalid PKCS#7 padding.
bool craftEcbBlock(const uint8_t plain16[16], uint8_t out16[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  const bool ok = mbedtls_aes_setkey_enc(&ctx, midea::kEncKey.data(), 128) == 0 &&
                  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, plain16, out16) == 0;
  mbedtls_aes_free(&ctx);
  return ok;
}

// KAT digests generated with Python hashlib 2026-09-05.
struct Kat {
  size_t len;
  const char* md5Hex;
  const char* sha256Hex;
};

constexpr Kat kKats[] = {
    {0, "d41d8cd98f00b204e9800998ecf8427e", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {1, "8666683506aacd900bbd5a74ac4edf68", "084fed08b978af4d7d196a7446a86b58009e636b611db16211b65a9aadff29c5"},
    {55, "52c0e574e1198de5fe3f8f11440dcb1b", "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b"},
    {56, "46c9907fc908ee68b1e7b8e71286a518", "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27"},
    {57, "1c805dd236c35cab25fcb1bc73802c51", "35df609437dcfea3279283ab79fd554e2bf78f8f7ae2de532d8ee300b09e8f73"},
    {63, "a62f6d59e837867693f042f5b8f5a236", "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055"},
    {64, "7160b8fb5e9e4023d549c3971fbaeead", "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241"},
    {65, "70bd662e7aefbda85a0f7244167b7897", "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e"},
    {71, "26e7e113db273c68b0fc3ce4660fc5e2", "54cac60524e8d20657ff88ee3951e22a506f3d8dc22c8fa848ce494672cb0d29"},
    {1000, "10046f077f2082ac19676b8079f1cb1a", "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371"},
};

} // namespace

int main() {
  // --- KAT digests over the pattern data (padding boundaries) ---
  for (const auto& kat : kKats) {
    std::vector<uint8_t> data(kat.len);
    fillPattern(data.data(), kat.len);
    uint8_t md5Digest[midea::kMd5DigestSize];
    uint8_t shaDigest[midea::kSha256DigestSize];
    midea::md5(data.data(), data.size(), md5Digest);
    midea::sha256(data.data(), data.size(), shaDigest);
    check(hexEq(md5Digest, sizeof(md5Digest), kat.md5Hex),
          std::string("md5 pattern len=" + std::to_string(kat.len)).c_str());
    check(hexEq(shaDigest, sizeof(shaDigest), kat.sha256Hex),
          std::string("sha256 pattern len=" + std::to_string(kat.len)).c_str());
  }

  // Classic RFC "abc" vectors.
  {
    uint8_t digest[midea::kMd5DigestSize];
    midea::md5("abc", 3, digest);
    check(hexEq(digest, sizeof(digest), "900150983cd24fb0d6963f7d28e17f72"), "md5(\"abc\")");
    uint8_t shaDigest[midea::kSha256DigestSize];
    midea::sha256("abc", 3, shaDigest);
    check(hexEq(shaDigest, sizeof(shaDigest), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "sha256(\"abc\")");
  }

  // --- Incremental update must match one-shot (streaming across blocks) ---
  {
    std::vector<uint8_t> data(200);
    fillPattern(data.data(), data.size());
    uint8_t oneShot[midea::kSha256DigestSize];
    midea::sha256(data.data(), data.size(), oneShot);
    midea::Sha256 ctx;
    for (size_t off = 0; off < data.size(); off += 37) {
      const size_t n = (data.size() - off < 37) ? data.size() - off : 37;
      ctx.update(data.data() + off, n);
    }
    uint8_t streamed[midea::kSha256DigestSize];
    ctx.final(streamed);
    check(std::memcmp(oneShot, streamed, sizeof(streamed)) == 0, "sha256 streaming == one-shot");
    // Reuse after final(): context must be back at IV.
    uint8_t again[midea::kSha256DigestSize];
    ctx.update(data.data(), data.size());
    ctx.final(again);
    check(std::memcmp(oneShot, again, sizeof(again)) == 0, "sha256 context reuse after final");
  }

  // --- Security vectors from crypto.txt ---
  const auto vectors = loadVectors("vectors/crypto.txt");
  if (vectors.empty()) return 1;
  unsigned seen = 0;
  for (const auto& [name, fields] : vectors) {
    const auto valueIt = fields.find("value");
    if (name == "sign_key" && valueIt != fields.end()) {
      std::vector<uint8_t> expected;
      parseHex(valueIt->second, expected);
      check(expected.size() == midea::kSignKeyLen &&
                std::memcmp(expected.data(), midea::kSignKey, expected.size()) == 0,
            "kSignKey matches SIGN_KEY");
      ++seen;
      continue;
    }
    if (name == "enc_key" && valueIt != fields.end()) {
      check(hexEq(midea::kEncKey.data(), midea::kEncKey.size(), valueIt->second), "kEncKey == md5(SIGN_KEY)");
      uint8_t digest[midea::kMd5DigestSize];
      midea::md5(midea::kSignKey, midea::kSignKeyLen, digest);
      check(hexEq(digest, sizeof(digest), valueIt->second), "runtime md5(SIGN_KEY) == fixture");
      ++seen;
      continue;
    }
    const auto inputIt = fields.find("input");
    const auto expectedIt = fields.find("expected");
    if (inputIt == fields.end() || expectedIt == fields.end()) continue;
    std::vector<uint8_t> input;
    if (!parseHex(inputIt->second, input)) {
      check(false, name + ": malformed input");
      continue;
    }
    if (name.rfind("sign_", 0) == 0) {
      uint8_t digest[midea::kMd5DigestSize];
      midea::sign(input.data(), input.size(), digest);
      check(hexEq(digest, sizeof(digest), expectedIt->second), "sign(" + name + ")");
      ++seen;
    } else if (name.rfind("udpid_", 0) == 0) {
      uint8_t got[16];
      midea::udpid(input.data(), input.size(), got);
      check(hexEq(got, sizeof(got), expectedIt->second), "udpid(" + name + ")");
      ++seen;
    } else if (name.rfind("aes_ecb", 0) == 0) {
      std::vector<uint8_t> cipher(midea::aesPkcs7PaddedSize(input.size()));
      size_t cipherLen = 0;
      const bool enc = midea::encryptAesPkcs7(input.data(), input.size(), cipher.data(), cipher.size(), &cipherLen);
      check(enc && hexEq(cipher.data(), cipherLen, expectedIt->second), "encryptAesPkcs7(" + name + ")");
      std::vector<uint8_t> plain(cipher.empty() ? 1 : cipher.size());
      size_t plainLen = 0;
      const bool dec = midea::decryptAesPkcs7(cipher.data(), cipherLen, plain.data(), plain.size(), &plainLen);
      check(dec && plainLen == input.size() && std::memcmp(plain.data(), input.data(), input.size()) == 0,
            "decryptAesPkcs7(" + name + ")");
      ++seen;
    } else if (name.rfind("aes_cbc", 0) == 0) {
      std::vector<uint8_t> key, expectedBytes;
      const auto keyIt = fields.find("key");
      if (keyIt == fields.end() || !parseHex(keyIt->second, key) || !parseHex(expectedIt->second, expectedBytes)) {
        check(false, name + ": malformed key/expected");
        continue;
      }
      std::vector<uint8_t> cipher(input.size()), plain(input.size());
      check(midea::encryptAesCbc(key.data(), key.size(), input.data(), input.size(), cipher.data(), cipher.size()) &&
                hexEq(cipher.data(), cipher.size(), expectedIt->second),
            "encryptAesCbc(" + name + ")");
      // Decrypt the fixture ciphertext (not our own output) back to input.
      check(midea::decryptAesCbc(key.data(), key.size(), expectedBytes.data(), expectedBytes.size(), plain.data(),
                                 plain.size()) &&
                std::memcmp(plain.data(), input.data(), input.size()) == 0,
            "decryptAesCbc(" + name + ")");
      ++seen;
    }
  }
  check(seen == 12, "consumed sign_key/enc_key/2 sign/2 udpid/3 ecb/3 cbc vectors");

  // --- AES-ECB PKCS#7: round-trip across pad boundaries 0..40 ---
  for (size_t len = 0; len <= 40; ++len) {
    std::vector<uint8_t> data(len ? len : 1);
    fillPattern(data.data(), len);
    const size_t cap = midea::aesPkcs7PaddedSize(len);
    const size_t pad = 16 - (len % 16);
    check(cap == len + pad && pad >= 1 && pad <= 16, "aesPkcs7PaddedSize len=" + std::to_string(len));
    std::vector<uint8_t> cipher(cap);
    size_t cipherLen = 0;
    check(midea::encryptAesPkcs7(data.data(), len, cipher.data(), cap, &cipherLen) && cipherLen == cap,
          "encryptAesPkcs7 len=" + std::to_string(len));
    size_t dummy = 0;
    check(!midea::encryptAesPkcs7(data.data(), len, cipher.data(), cap - 1, &dummy),
          "encryptAesPkcs7 rejects small outCap len=" + std::to_string(len));
    std::vector<uint8_t> plain(cap);
    size_t plainLen = 0;
    check(midea::decryptAesPkcs7(cipher.data(), cipherLen, plain.data(), plain.size(), &plainLen) && plainLen == len &&
              std::memcmp(plain.data(), data.data(), len) == 0,
          "decryptAesPkcs7 round-trip len=" + std::to_string(len));
    check(!midea::decryptAesPkcs7(cipher.data(), cipherLen, plain.data(), cap - 1, &plainLen),
          "decryptAesPkcs7 rejects small outCap len=" + std::to_string(len));
  }

  // --- AES-ECB PKCS#7 validation (ciphertext crafted via raw mbedtls) ---
  {
    size_t outLen = 0;
    uint8_t plain[64];
    std::vector<uint8_t> cipher(32, 0x5a);
    check(!midea::decryptAesPkcs7(cipher.data(), 0, plain, sizeof(plain), &outLen), "decrypt rejects len 0");
    check(!midea::decryptAesPkcs7(cipher.data(), 17, plain, sizeof(plain), &outLen), "decrypt rejects len%16!=0");

    auto craftReject = [&](const uint8_t block[16], const std::string& what) {
      uint8_t cb[16];
      if (!craftEcbBlock(block, cb)) {
        check(false, what + " (craft)");
        return;
      }
      check(!midea::decryptAesPkcs7(cb, 16, plain, sizeof(plain), &outLen), what);
    };
    uint8_t block[16];
    fillPattern(block, 16);
    block[15] = 0x00;
    craftReject(block, "decrypt rejects pad 0");
    fillPattern(block, 16);
    block[15] = 0x11;
    craftReject(block, "decrypt rejects pad 17");
    fillPattern(block, 16);
    block[15] = 0x04;
    block[12] = 0x03; // pad run of 4 must all be 0x04
    craftReject(block, "decrypt rejects pad mismatch");

    auto craftAccept = [&](const uint8_t block[16], size_t expectLen, const std::string& what) {
      uint8_t cb[16];
      if (!craftEcbBlock(block, cb)) {
        check(false, what + " (craft)");
        return;
      }
      check(midea::decryptAesPkcs7(cb, 16, plain, sizeof(plain), &outLen) && outLen == expectLen, what);
    };
    fillPattern(block, 16);
    block[15] = 0x01;
    craftAccept(block, 15, "decrypt accepts pad 1");
    uint8_t padBlock[16];
    std::memset(padBlock, 0x10, sizeof(padBlock));
    craftAccept(padBlock, 0, "decrypt accepts full pad block (plaintext len 0)");
  }

  // --- AES-CBC zero-IV: round-trips, chaining, rejection ---
  {
    uint8_t key16[16], key32[32], data[64], out[64], back[64];
    fillPattern(key16, sizeof(key16));
    for (size_t i = 0; i < sizeof(key32); ++i)
      key32[i] = static_cast<uint8_t>(200 - i);
    uint32_t lcg = 0x12345678u;
    for (size_t i = 0; i < sizeof(data); ++i) {
      lcg = lcg * 1664525u + 1013904223u;
      data[i] = static_cast<uint8_t>(lcg >> 16);
    }
    for (size_t len : {size_t(16), size_t(32), size_t(48)}) {
      for (size_t k = 0; k < 2; ++k) {
        const uint8_t* key = k ? key32 : key16;
        const size_t keyLen = k ? 32 : 16;
        const std::string tag = "len=" + std::to_string(len) + " key=" + std::to_string(keyLen * 8);
        check(midea::encryptAesCbc(key, keyLen, data, len, out, sizeof(out)), "cbc encrypt " + tag);
        check(midea::decryptAesCbc(key, keyLen, out, len, back, sizeof(back)), "cbc decrypt " + tag);
        check(std::memcmp(back, data, len) == 0, "cbc round-trip " + tag);
      }
    }
    // CBC must actually chain: two identical blocks must not repeat.
    uint8_t same[32], cout32[32];
    fillPattern(same, 16);
    std::memcpy(same + 16, same, 16);
    check(midea::encryptAesCbc(key16, 16, same, 32, cout32, sizeof(cout32)) &&
              std::memcmp(cout32, cout32 + 16, 16) != 0,
          "cbc chains identical blocks");
    // Zero-length passes are accepted and write nothing (pycryptodome parity).
    check(midea::encryptAesCbc(key16, 16, data, 0, out, 0), "cbc accepts len 0");
    check(!midea::encryptAesCbc(key16, 16, data, 33, out, sizeof(out)), "cbc rejects len%16!=0");
    check(!midea::encryptAesCbc(key32, 24, data, 32, out, sizeof(out)), "cbc rejects 192-bit key");
    check(!midea::encryptAesCbc(key16, 16, data, 32, out, 31), "cbc rejects small outCap");
    check(!midea::decryptAesCbc(key16, 0, data, 32, out, sizeof(out)), "cbc rejects empty key");
    check(!midea::decryptAesCbc(key16, 17, data, 32, out, sizeof(out)), "cbc rejects 136-bit key");
  }

  if (gFailures != 0) {
    std::printf("%u checks FAILED\n", gFailures);
    return 1;
  }
  std::printf("security: all checks passed\n");
  return 0;
}
