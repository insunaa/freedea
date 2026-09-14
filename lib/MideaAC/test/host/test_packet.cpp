// Host test for the V2 LAN packet layer (midea::encodeV2Packet/decodeV2Packet,
// port of _Packet lan.py 686-757): byte-exact frozen-clock encode golden,
// decode golden, structural header checks, round-trips and rejection paths.
// Golden vectors come from the msmart reference via vectors/lan.txt.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "Packet.h"
#include "Security.h"

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

unsigned gFailures = 0;

void check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++gFailures;
}

using Fields = std::unordered_map<std::string, std::string>;

// Parses the shared vector format (same loader as test_security.cpp).
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
      while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
      fields[key] = value;
    }
  }
  if (have) vectors.emplace_back(name, fields);
  return vectors;
}

const Fields* findVector(const std::vector<std::pair<std::string, Fields>>& vectors, const char* name) {
  for (const auto& entry : vectors) {
    if (entry.first == name) return &entry.second;
  }
  return nullptr;
}

uint64_t readLe64(const uint8_t* p) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

} // namespace

int main() {
  const auto vectors = loadVectors("vectors/lan.txt");
  const Fields* fDecode = findVector(vectors, "v2_decode");
  const Fields* fRoundtrip = findVector(vectors, "v2_roundtrip");
  const Fields* fGolden = findVector(vectors, "v2_encode_golden");
  check(fDecode && fRoundtrip && fGolden, "lan.txt contains v2_decode/v2_roundtrip/v2_encode_golden");
  if (!fDecode || !fRoundtrip || !fGolden) {
    return gFailures == 0 ? 1 : static_cast<int>(gFailures);
  }

  std::vector<uint8_t> decodePacket, decodeFrame, goldenFrame, goldenTs, goldenPacket, rtFrame;
  bool parsed = parseHex(fDecode->at("packet"), decodePacket) && parseHex(fDecode->at("expected_frame"), decodeFrame) &&
                parseHex(fGolden->at("frame"), goldenFrame) && parseHex(fGolden->at("timestamp"), goldenTs) &&
                parseHex(fGolden->at("expected_packet"), goldenPacket) && parseHex(fRoundtrip->at("frame"), rtFrame);
  check(parsed, "all hex fields parse");
  check(goldenTs.size() == midea::kV2PacketTimestampLen, "golden timestamp is 8 bytes");
  if (!parsed) {
    std::printf("FAIL: vector parse errors\n");
    return 1;
  }
  const uint64_t goldenDeviceId = std::strtoull(fGolden->at("device_id").c_str(), nullptr, 10);
  const uint64_t rtDeviceId = std::strtoull(fRoundtrip->at("device_id").c_str(), nullptr, 10);

  // --- buildV2Timestamp: golden layout is 2a3b3a171d061914, hundredths first
  {
    uint8_t ts[midea::kV2PacketTimestampLen];
    midea::buildV2Timestamp(ts, 2025, 6, 29, 23, 58, 59, 42);
    check(std::memcmp(ts, goldenTs.data(), sizeof(ts)) == 0, "buildV2Timestamp matches golden bytes");
  }

  // --- Byte-exact encode against the frozen-clock golden packet
  std::vector<uint8_t> pkt(midea::v2PacketSize(goldenFrame.size()));
  size_t pktLen = 0;
  bool ok = midea::encodeV2Packet(goldenFrame.data(), goldenFrame.size(), goldenDeviceId, goldenTs.data(), pkt.data(),
                                  pkt.size(), &pktLen);
  check(ok && pktLen == goldenPacket.size(), "encode golden: success and size");
  check(ok && std::memcmp(pkt.data(), goldenPacket.data(), goldenPacket.size()) == 0, "encode golden: byte exact");

  // Capacity exactly one byte short must fail.
  if (pkt.size() > 1) {
    check(!midea::encodeV2Packet(goldenFrame.data(), goldenFrame.size(), goldenDeviceId, goldenTs.data(), pkt.data(),
                                 pkt.size() - 1, &pktLen),
          "encode rejects outCap one byte short");
  }

  // --- Decode golden packet (captured from a real device; byte 7 differs
  // from what encode writes, decode must ignore it).
  std::vector<uint8_t> frame(midea::v2PacketSize(decodePacket.size()));
  size_t frameLen = 0;
  ok = midea::decodeV2Packet(decodePacket.data(), decodePacket.size(), frame.data(), frame.size(), &frameLen);
  check(ok && frameLen == decodeFrame.size() && std::memcmp(frame.data(), decodeFrame.data(), frameLen) == 0,
        "decode golden packet yields expected frame");

  // Trailing garbage beyond the declared length is ignored.
  std::vector<uint8_t> padded = decodePacket;
  padded.insert(padded.end(), 7, 0xAB);
  frameLen = 0;
  ok = midea::decodeV2Packet(padded.data(), padded.size(), frame.data(), frame.size(), &frameLen);
  check(ok && frameLen == decodeFrame.size() && std::memcmp(frame.data(), decodeFrame.data(), frameLen) == 0,
        "decode ignores trailing bytes after declared length");

  // --- Rejection paths on the golden packet
  std::vector<uint8_t> bad;
  frameLen = 0;
  check(!midea::decodeV2Packet(decodePacket.data(), 5, frame.data(), frame.size(), &frameLen), "decode rejects len<6");

  bad = decodePacket;
  bad[0] = 0xA5;
  check(!midea::decodeV2Packet(bad.data(), bad.size(), frame.data(), frame.size(), &frameLen),
        "decode rejects bad start");

  bad = decodePacket;
  bad[bad.size() - 1] ^= 0xFF; // corrupt md5 tail
  check(!midea::decodeV2Packet(bad.data(), bad.size(), frame.data(), frame.size(), &frameLen),
        "decode rejects bad md5");

  bad = decodePacket;
  bad[40] ^= 0xFF; // corrupt ciphertext, md5 no longer matches
  check(!midea::decodeV2Packet(bad.data(), bad.size(), frame.data(), frame.size(), &frameLen),
        "decode rejects corrupted ciphertext");

  bad = decodePacket;
  bad[4] = 0x10;
  bad[5] = 0x00; // declared 16 < kV2PacketMinLen
  check(!midea::decodeV2Packet(bad.data(), bad.size(), frame.data(), frame.size(), &frameLen),
        "decode rejects declared length below minimum");

  bad = decodePacket;
  bad[4] = 0xFF;
  bad[5] = 0xFF; // declared 65535 > available
  check(!midea::decodeV2Packet(bad.data(), bad.size(), frame.data(), frame.size(), &frameLen),
        "decode rejects truncated packet");

  // Frame buffer smaller than the ciphertext must fail.
  std::vector<uint8_t> tiny(16);
  frameLen = 0;
  check(!midea::decodeV2Packet(decodePacket.data(), decodePacket.size(), tiny.data(), tiny.size(), &frameLen),
        "decode rejects undersized frame buffer");

  // --- Round-trip vector: structure + ciphertext + md5 checks
  {
    uint8_t ts[midea::kV2PacketTimestampLen];
    midea::buildV2Timestamp(ts, 2025, 6, 29, 23, 58, 59, 42);
    std::vector<uint8_t> rt(midea::v2PacketSize(rtFrame.size()));
    size_t rtLen = 0;
    ok = midea::encodeV2Packet(rtFrame.data(), rtFrame.size(), rtDeviceId, ts, rt.data(), rt.size(), &rtLen);
    check(ok, "roundtrip encode succeeds");
    if (ok) {
      const size_t cipherLen = rtLen - midea::kV2PacketHeaderLen - midea::kV2PacketHashLen;
      check(rt[0] == 0x5A && rt[1] == 0x5A, "header start 5A5A");
      check(rt[2] == 0x01 && rt[3] == 0x11, "header message type 01 11");
      check((static_cast<size_t>(rt[4]) | (static_cast<size_t>(rt[5]) << 8)) == rtLen, "LE16 size equals total");
      check(rt[6] == 0x20 && rt[7] == 0x00, "magic 20 00");
      bool msgIdZero = true;
      for (size_t i = 8; i < 12; ++i)
        msgIdZero = msgIdZero && rt[i] == 0;
      check(msgIdZero, "message id zero");
      check(std::memcmp(&rt[12], ts, 8) == 0, "timestamp at [12:20]");
      check(readLe64(&rt[20]) == rtDeviceId, "device id LE64 at [20:28]");
      bool reservedZero = true;
      for (size_t i = 28; i < 40; ++i)
        reservedZero = reservedZero && rt[i] == 0;
      check(reservedZero, "reserved [28:40] zero");
      // Same frame and same ENC_KEY as the golden vector: ciphertext matches.
      const size_t goldenCipherLen = goldenPacket.size() - midea::kV2PacketHeaderLen - midea::kV2PacketHashLen;
      check(cipherLen == goldenCipherLen &&
                std::memcmp(&rt[midea::kV2PacketHeaderLen], &goldenPacket[midea::kV2PacketHeaderLen], cipherLen) == 0,
            "ciphertext matches golden ciphertext");
      uint8_t digest[midea::kMd5DigestSize];
      midea::sign(rt.data(), rtLen - midea::kV2PacketHashLen, digest);
      check(std::memcmp(digest, &rt[rtLen - midea::kV2PacketHashLen], midea::kMd5DigestSize) == 0, "md5 tail verifies");

      size_t rxLen = 0;
      ok = midea::decodeV2Packet(rt.data(), rtLen, frame.data(), frame.size(), &rxLen);
      check(ok && rxLen == rtFrame.size() && std::memcmp(frame.data(), rtFrame.data(), rxLen) == 0,
            "roundtrip decode equals frame");
    }
  }

  std::printf("%s: %u failure(s)\n", gFailures == 0 ? "PASS" : "FAIL", gFailures);
  return gFailures == 0 ? 0 : 1;
}
