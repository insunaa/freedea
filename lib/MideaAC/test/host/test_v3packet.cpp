// Host test for the V3 LAN packet layer (midea::V3Packet.*, port of
// _LanProtocolV3 lan.py 146-426): byte-exact frozen-pad encode golden (mirrors
// test_encode_packet_v3_roundtrip), decode golden (test_decode_v3_packet),
// handshake request golden, local-key derivation, and rejection paths.
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
#include "Sha256.h"
#include "V3Packet.h"

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

// Parses the shared vector format (same loader as test_packet.cpp).
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

uint16_t readBe16(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

} // namespace

int main() {
  const auto vectors = loadVectors("vectors/lan.txt");
  const Fields* fEnc = findVector(vectors, "v3_encode_golden");
  const Fields* fDec = findVector(vectors, "v3_decode");
  const Fields* fRt = findVector(vectors, "v3_roundtrip");
  const Fields* fHs = findVector(vectors, "v3_handshake_golden");
  const Fields* fLk = findVector(vectors, "v3_local_key");
  check(fEnc && fDec && fRt && fHs && fLk, "lan.txt contains all v3 vectors");
  if (!fEnc || !fDec || !fRt || !fHs || !fLk) {
    return gFailures == 0 ? 1 : static_cast<int>(gFailures);
  }

  std::vector<uint8_t> encData, encKey, encPad, encGolden, decPacket, decKey, decPayload, decFrame, rtFrame, hsToken,
      hsGolden, lkKey, lkData, lkExpected;
  bool parsed = parseHex(fEnc->at("data"), encData) && parseHex(fEnc->at("local_key"), encKey) &&
                parseHex(fEnc->at("pad"), encPad) && parseHex(fEnc->at("expected_packet"), encGolden) &&
                parseHex(fDec->at("packet"), decPacket) && parseHex(fDec->at("local_key"), decKey) &&
                parseHex(fDec->at("expected_payload"), decPayload) && parseHex(fDec->at("expected_frame"), decFrame) &&
                parseHex(fRt->at("frame"), rtFrame) && parseHex(fHs->at("token"), hsToken) &&
                parseHex(fHs->at("expected_packet"), hsGolden) && parseHex(fLk->at("key"), lkKey) &&
                parseHex(fLk->at("handshake_data"), lkData) && parseHex(fLk->at("expected_local_key"), lkExpected);
  check(parsed, "all hex fields parse");
  if (!parsed) {
    std::printf("FAIL: vector parse errors\n");
    return 1;
  }
  const uint16_t encId = static_cast<uint16_t>(std::strtoul(fEnc->at("packet_id").c_str(), nullptr, 10));
  const uint16_t rtId = static_cast<uint16_t>(std::strtoul(fRt->at("packet_id").c_str(), nullptr, 10));
  const uint64_t rtDeviceId = std::strtoull(fRt->at("device_id").c_str(), nullptr, 10);
  const uint16_t hsId = static_cast<uint16_t>(std::strtoul(fHs->at("packet_id").c_str(), nullptr, 10));

  // --- Encrypted request golden: byte-exact encode with fixed pad
  check(midea::v3PadSize(encData.size()) == encPad.size(), "golden data pad matches vector pad length");
  check(midea::v3EncryptedPacketSize(encData.size()) == encGolden.size(), "golden size matches helper");
  {
    std::vector<uint8_t> out(midea::v3EncryptedPacketSize(encData.size()));
    size_t outLen = 0;
    bool ok = midea::encodeV3EncryptedRequest(encKey.data(), encId, encData.data(), encData.size(), encPad.data(),
                                              out.data(), out.size(), &outLen);
    check(ok && outLen == encGolden.size() && std::memcmp(out.data(), encGolden.data(), outLen) == 0,
          "encode golden: byte exact");
    check(out[0] == 0x83 && out[1] == 0x70 && out[4] == 0x20, "golden header start/magic");
    check(readBe16(&out[2]) == encData.size() + encPad.size() + midea::kV3SignLen, "BE16 size = data+pad+32");
    check(out[5] == (encPad.size() << 4 | 0x06), "pad<<4|type byte = 0x66");
    check(midea::v3PacketType(out.data()) == midea::V3PacketType::EncryptedRequest, "type nibble is request (6)");

    // Rejections on inputs.
    size_t dummy = 0;
    check(!midea::encodeV3EncryptedRequest(encKey.data(), encId, encData.data(), encData.size(), encPad.data(),
                                           out.data(), out.size() - 1, &dummy),
          "encode rejects outCap one byte short");
    check(!midea::encodeV3EncryptedRequest(encKey.data(), encId, encData.data(), encData.size(), nullptr, out.data(),
                                           out.size(), &dummy),
          "encode rejects null pad bytes when pad > 0");
    check(!midea::encodeV3EncryptedRequest(nullptr, encId, encData.data(), encData.size(), encPad.data(), out.data(),
                                           out.size(), &dummy),
          "encode rejects null key");

    // Round-trip through the direct response handler (encoder output is type
    // 6, so the dispatcher must refuse it).
    std::vector<uint8_t> back(outLen);
    size_t backLen = 0;
    ok = midea::decodeV3EncryptedResponse(encKey.data(), out.data(), outLen, back.data(), back.size(), &backLen);
    check(ok && backLen == encData.size() && std::memcmp(back.data(), encData.data(), backLen) == 0,
          "golden decode via handler: equals data");
    check(!midea::decodeV3Packet(encKey.data(), out.data(), outLen, back.data(), back.size(), &backLen),
          "dispatcher rejects type 6");

    // Corruption and key failures on the golden packet.
    std::vector<uint8_t> bad;
    bad = encGolden;
    bad[10] ^= 0xFF;
    check(!midea::decodeV3EncryptedResponse(encKey.data(), bad.data(), bad.size(), back.data(), back.size(), &backLen),
          "decode rejects corrupted ciphertext (hash mismatch)");
    bad = encGolden;
    bad.back() ^= 0xFF;
    check(!midea::decodeV3EncryptedResponse(encKey.data(), bad.data(), bad.size(), back.data(), back.size(), &backLen),
          "decode rejects corrupted hash");
    std::vector<uint8_t> wrongKey(encKey.size(), 0xA5);
    check(!midea::decodeV3EncryptedResponse(wrongKey.data(), encGolden.data(), encGolden.size(), back.data(),
                                            back.size(), &backLen),
          "decode rejects wrong local key");
    check(!midea::decodeV3EncryptedResponse(encKey.data(), encGolden.data(), encGolden.size() - 1, back.data(),
                                            back.size(), &backLen),
          "decode rejects truncated packet");
  }

  // --- Pad-zero edge: dataLen 14 -> pad 0, encode needs no pad bytes.
  {
    const std::vector<uint8_t> data(14, 0x42);
    std::vector<uint8_t> out(midea::v3EncryptedPacketSize(data.size()));
    size_t outLen = 0;
    bool ok = midea::encodeV3EncryptedRequest(encKey.data(), 1, data.data(), data.size(), nullptr, out.data(),
                                              out.size(), &outLen);
    check(ok && out[5] == 0x06, "pad-zero encode: type 6, pad nibble 0");
    std::vector<uint8_t> back(outLen);
    size_t backLen = 0;
    ok = midea::decodeV3EncryptedResponse(encKey.data(), out.data(), outLen, back.data(), back.size(), &backLen);
    check(ok && backLen == data.size() && std::memcmp(back.data(), data.data(), backLen) == 0,
          "pad-zero round-trip: slices to end");
  }

  // --- Decode golden (test_decode_v3_packet) chained into the V2 decoder
  check(midea::v3PacketType(decPacket.data()) == midea::V3PacketType::EncryptedResponse, "decode golden type is 3");
  {
    std::vector<uint8_t> payload(decPacket.size());
    size_t payloadLen = 0;
    bool ok = midea::decodeV3Packet(decKey.data(), decPacket.data(), decPacket.size(), payload.data(), payload.size(),
                                    &payloadLen);
    check(ok && payloadLen == decPayload.size() && std::memcmp(payload.data(), decPayload.data(), payloadLen) == 0,
          "decode golden: payload matches (pad nibble 6 stripped)");

    // Trailing garbage beyond size+8 is ignored.
    std::vector<uint8_t> padded = decPacket;
    padded.insert(padded.end(), 5, 0xAB);
    payloadLen = 0;
    ok =
        midea::decodeV3Packet(decKey.data(), padded.data(), padded.size(), payload.data(), payload.size(), &payloadLen);
    check(ok && payloadLen == decPayload.size(), "decode ignores trailing bytes");

    // The extracted payload is itself a V2 packet carrying the golden frame.
    std::vector<uint8_t> frame(midea::v2PacketSize(decPayload.size()));
    size_t frameLen = 0;
    ok = midea::decodeV2Packet(decPayload.data(), decPayload.size(), frame.data(), frame.size(), &frameLen);
    check(ok && frameLen == decFrame.size() && std::memcmp(frame.data(), decFrame.data(), frameLen) == 0,
          "decoded payload decodes as V2 packet to golden frame");

    // Rejections.
    check(!midea::decodeV3Packet(nullptr, decPacket.data(), decPacket.size(), payload.data(), payload.size(),
                                 &payloadLen),
          "decode rejects null key for type 3");
    const size_t cipherLen = decPacket.size() - midea::kV3HeaderLen - midea::kV3SignLen;
    check(!midea::decodeV3Packet(decKey.data(), decPacket.data(), decPacket.size(), payload.data(), cipherLen - 1,
                                 &payloadLen),
          "decode rejects undersized payload buffer");
    std::vector<uint8_t> bad = decPacket;
    bad[0] = 0x84;
    check(!midea::decodeV3Packet(decKey.data(), bad.data(), bad.size(), payload.data(), payload.size(), &payloadLen),
          "decode rejects bad start");
    bad = decPacket;
    bad[4] = 0x21;
    check(!midea::decodeV3Packet(decKey.data(), bad.data(), bad.size(), payload.data(), payload.size(), &payloadLen),
          "decode rejects bad magic byte");
    check(!midea::decodeV3Packet(decKey.data(), decPacket.data(), 5, payload.data(), payload.size(), &payloadLen),
          "decode rejects len < 6");
  }

  // --- Round-trip (test_encode_packet_v3_roundtrip): frame -> V2 -> V3 -> V2
  // -> frame with the golden frame and local key.
  {
    uint8_t ts[midea::kV2PacketTimestampLen] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> v2(midea::v2PacketSize(rtFrame.size()));
    size_t v2Len = 0;
    bool ok = midea::encodeV2Packet(rtFrame.data(), rtFrame.size(), rtDeviceId, ts, v2.data(), v2.size(), &v2Len);
    check(ok, "roundtrip: V2 encode");
    const size_t padLen = midea::v3PadSize(v2Len);
    std::vector<uint8_t> pad(padLen, 0x5C);
    std::vector<uint8_t> v3(midea::v3EncryptedPacketSize(v2Len));
    size_t v3Len = 0;
    ok = midea::encodeV3EncryptedRequest(decKey.data(), rtId, v2.data(), v2Len, pad.data(), v3.data(), v3.size(),
                                         &v3Len);
    check(ok && v3Len == midea::v3EncryptedPacketSize(v2Len), "roundtrip: V3 encode size");
    check(readBe16(&v3[2]) == v2Len + padLen + midea::kV3SignLen, "roundtrip: BE16 size field");
    check(v3[5] == (padLen << 4 | 0x06), "roundtrip: pad<<4|type byte");

    std::vector<uint8_t> rxV2(v3Len);
    size_t rxV2Len = 0;
    ok = midea::decodeV3EncryptedResponse(decKey.data(), v3.data(), v3Len, rxV2.data(), rxV2.size(), &rxV2Len);
    check(ok && rxV2Len == v2Len && std::memcmp(rxV2.data(), v2.data(), rxV2Len) == 0,
          "roundtrip: V3 decode equals V2 packet");
    std::vector<uint8_t> frame(v2.size());
    size_t frameLen = 0;
    ok = midea::decodeV2Packet(rxV2.data(), rxV2Len, frame.data(), frame.size(), &frameLen);
    check(ok && frameLen == rtFrame.size() && std::memcmp(frame.data(), rtFrame.data(), frameLen) == 0,
          "roundtrip: V2 decode equals original frame");
  }

  // --- Handshake request golden
  check(midea::v3HandshakePacketSize(hsToken.size()) == hsGolden.size(), "handshake size helper");
  {
    std::vector<uint8_t> out(midea::v3HandshakePacketSize(hsToken.size()));
    size_t outLen = 0;
    bool ok = midea::encodeV3HandshakeRequest(hsId, hsToken.data(), hsToken.size(), out.data(), out.size(), &outLen);
    check(ok && outLen == hsGolden.size() && std::memcmp(out.data(), hsGolden.data(), outLen) == 0,
          "handshake golden: byte exact");
    check(readBe16(&out[2]) == hsToken.size(), "handshake size field = token length");
    check(out[5] == 0x00, "handshake type byte 0");
    check(midea::v3PacketType(hsGolden.data()) == midea::V3PacketType::HandshakeRequest, "type nibble is 0");
    size_t dummy = 0;
    check(!midea::encodeV3HandshakeRequest(hsId, hsToken.data(), hsToken.size(), out.data(), out.size() - 1, &dummy),
          "handshake rejects outCap one byte short");
    // Requests are never received.
    std::vector<uint8_t> payload(out.size());
    check(!midea::decodeV3Packet(nullptr, hsGolden.data(), hsGolden.size(), payload.data(), payload.size(), &dummy),
          "dispatcher rejects handshake request");
  }

  // --- Synthesized handshake response (type 1): payload returned raw from [8:]
  {
    const uint8_t resp[] = {0x83, 0x70, 0x00, 0x06, 0x20, 0x01, 0x00, 0x0A, 'A', 'B', 'C', 'D', 'E', 'F'};
    uint8_t payload[8];
    size_t payloadLen = 0;
    bool ok = midea::decodeV3Packet(nullptr, resp, sizeof(resp), payload, sizeof(payload), &payloadLen);
    check(ok && payloadLen == 6 && std::memcmp(payload, "ABCDEF", 6) == 0, "handshake response: raw payload");
    check(midea::v3PacketType(resp) == midea::V3PacketType::HandshakeResponse, "handshake response type nibble");
    check(!midea::decodeV3Packet(nullptr, resp, sizeof(resp) - 1, payload, sizeof(payload), &payloadLen),
          "handshake response: rejects truncated buffer");
    check(!midea::decodeV3Packet(nullptr, resp, sizeof(resp), payload, 5, &payloadLen),
          "handshake response: rejects undersized payload buffer");
  }

  // --- Local key derivation golden
  {
    uint8_t derived[midea::kV3LocalKeyLen];
    bool ok = midea::deriveV3LocalKey(lkKey.data(), lkData.data(), derived);
    check(ok && std::memcmp(derived, lkExpected.data(), lkExpected.size()) == 0, "local key golden matches");
    std::vector<uint8_t> bad = lkData;
    bad[10] ^= 0xFF;
    check(!midea::deriveV3LocalKey(lkKey.data(), bad.data(), derived), "local key rejects corrupted payload");
    bad = lkData;
    bad[40] ^= 0xFF;
    check(!midea::deriveV3LocalKey(lkKey.data(), bad.data(), derived), "local key rejects corrupted hash");
    std::vector<uint8_t> wrongKey(lkKey.size(), 0x5A);
    check(!midea::deriveV3LocalKey(wrongKey.data(), lkData.data(), derived), "local key rejects wrong key");
  }

  std::printf("%s: %u failure(s)\n", gFailures == 0 ? "PASS" : "FAIL", gFailures);
  return gFailures == 0 ? 0 : 1;
}
