// Host test for midea frame build/validate against golden vectors from
// msmart (see vectors/NOTES.md), plus edge cases verified against the
// Python implementation.
#include "Crc8.h"
#include "Frame.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

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

using Fields = std::unordered_map<std::string, std::string>;

// Parses the shared vector format: column-0 name, indented `field value`.
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
    if (kv >> key >> value) fields[key] = value;
  }
  if (have) vectors.emplace_back(name, fields);
  return vectors;
}

unsigned gFailures = 0;

void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++gFailures;
}

constexpr size_t kMaxFrame = midea::kFrameHeaderLen + midea::kFrameMaxDataLen + 1;

// Empty-data frame, cross-checked against Frame(0xAC, 0x03).tobytes(b""):
// "aa0aac0000000000000347".
constexpr uint8_t kEmptyAcQueryFrame[] = {0xAA, 0x0A, 0xAC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x47};

} // namespace

// Checksum is constexpr: must match the empty-frame checksum at compile time.
constexpr uint8_t kCsumBytes[] = {0x0A, 0xAC, 0x03};
static_assert(midea::frameChecksum(kCsumBytes, 3) == 0x47);
// And the CRC8 table composes with frames (payload + id is CRC8'd).
constexpr uint8_t kCrcProbe[] = {0x01};
static_assert(midea::crc8(kCrcProbe, 1) == 0x5E);

int main() {
  const auto vectors = loadVectors("vectors/frame.txt");
  if (vectors.empty()) return 1;

  // --- Build vectors: payload -> byte-exact frame ---
  for (const auto& [name, fields] : vectors) {
    const auto payloadIt = fields.find("payload");
    if (payloadIt == fields.end()) continue;
    std::vector<uint8_t> payload;
    if (!parseHex(payloadIt->second, payload)) {
      check(false, (name + ": malformed payload").c_str());
      continue;
    }
    uint8_t frame[kMaxFrame];
    const size_t len = midea::buildFrame(frame, sizeof(frame), midea::kDeviceTypeAc, midea::kFrameTypeQuery,
                                         payload.data(), payload.size());
    if (len != midea::kFrameHeaderLen + payload.size() + 1) {
      check(false, (name + ": build length").c_str());
      continue;
    }
    const std::string gotHex = [&] {
      std::string s;
      static const char* kHex = "0123456789abcdef";
      for (size_t i = 0; i < len; ++i) {
        s += kHex[frame[i] >> 4];
        s += kHex[frame[i] & 0xF];
      }
      return s;
    }();
    const auto frameIt = fields.find("frame");
    if (frameIt != fields.end()) {
      check(gotHex == frameIt->second, (name + ": built frame matches golden").c_str());
    } else {
      check(midea::validateFrame(frame, len, midea::kDeviceTypeAc), (name + ": built frame validates").c_str());
    }
  }

  // --- Validate-only vectors (frames not producible by buildFrame, e.g.
  // v2_roundtrip_frame carries byte 3 = 0x8d) ---
  for (const auto& [name, fields] : vectors) {
    const auto frameIt = fields.find("frame");
    if (frameIt == fields.end()) continue;
    std::vector<uint8_t> frame;
    if (!parseHex(frameIt->second, frame)) {
      check(false, (name + ": malformed frame").c_str());
      continue;
    }
    check(midea::validateFrame(frame.data(), frame.size(), midea::kDeviceTypeAc),
          (name + ": golden frame validates").c_str());
    check(!midea::validateFrame(frame.data(), frame.size(), 0xB0), (name + ": wrong device type rejected").c_str());
  }

  // --- Edge and negative cases ---
  uint8_t frame[kMaxFrame];
  const size_t emptyLen =
      midea::buildFrame(frame, sizeof(frame), midea::kDeviceTypeAc, midea::kFrameTypeQuery, nullptr, 0);
  check(emptyLen == sizeof(kEmptyAcQueryFrame) && std::memcmp(frame, kEmptyAcQueryFrame, emptyLen) == 0,
        "empty data -> aa0aac0000000000000347");
  check(midea::validateFrame(kEmptyAcQueryFrame, sizeof(kEmptyAcQueryFrame), midea::kDeviceTypeAc),
        "empty frame validates");

  // Length byte overflow: 246 payload bytes would not fit in the byte-1 length.
  static uint8_t bigData[midea::kFrameMaxDataLen + 1] = {};
  check(midea::buildFrame(frame, sizeof(frame), midea::kDeviceTypeAc, midea::kFrameTypeQuery, bigData,
                          midea::kFrameMaxDataLen + 1) == 0,
        "oversized payload rejected");
  check(midea::buildFrame(frame, kMaxFrame - 1, midea::kDeviceTypeAc, midea::kFrameTypeQuery, bigData,
                          midea::kFrameMaxDataLen) == 0,
        "small outCap rejected");

  // Corrupting any byte (after the start) must fail validation.
  const auto full = [&] {
    std::vector<uint8_t> payload;
    parseHex("418100ff03ff00020000000000000000000000000311f4", payload);
    std::vector<uint8_t> out(kMaxFrame);
    const size_t n = midea::buildFrame(out.data(), out.size(), midea::kDeviceTypeAc, midea::kFrameTypeQuery,
                                       payload.data(), payload.size());
    out.resize(n);
    return out;
  }();
  check(midea::validateFrame(full.data(), full.size(), midea::kDeviceTypeAc), "getstate frame validates");
  check(!midea::validateFrame(full.data(), midea::kFrameHeaderLen - 1, midea::kDeviceTypeAc),
        "frame below 10 bytes rejected");
  bool singleBitAllDetected = true;
  for (size_t bit = 8; bit < full.size() * 8; ++bit) { // skip start byte
    std::vector<uint8_t> bad = full;
    bad[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
    if (midea::validateFrame(bad.data(), bad.size(), midea::kDeviceTypeAc)) singleBitAllDetected = false;
  }
  check(singleBitAllDetected, "every single-bit corruption fails checksum/device check");

  if (gFailures != 0) {
    std::printf("%u checks FAILED\n", gFailures);
    return 1;
  }
  std::printf("frame: all checks passed\n");
  return 0;
}
