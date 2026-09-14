// Host test for midea discovery parsing (DISCOVERY_MSG constants, version
// detection, V2/V3 response struct parsing) against golden vectors generated
// by msmart itself (vectors/discovery.txt). Golden responses come from
// msmart's own test_discover.py samples; error vectors pin C++-hardened
// outcomes where upstream merely raises.
#include "Discovery.h"
#include "Security.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

std::string toHex(const uint8_t* data, size_t len) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0x0F]);
  }
  return out;
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

void check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++gFailures;
}

const char* errorName(midea::DiscoveryError err) {
  switch (err) {
    case midea::DiscoveryError::kNone:
      return "none";
    case midea::DiscoveryError::kTooShort:
      return "short";
    case midea::DiscoveryError::kDecryptFailed:
      return "decrypt";
    case midea::DiscoveryError::kBadName:
      return "name";
    case midea::DiscoveryError::kNameTooLong:
      return "name_too_long";
  }
  return "?";
}

// V2 uses the whole datagram; V3 strips the 8-byte header and 16-byte hash
// before the common slicing (mirrors discover.py).
struct View {
  const uint8_t* data;
  size_t len;
};

View viewFor(const std::vector<uint8_t>& frame, midea::DiscoveryVersion version) {
  if (version == midea::DiscoveryVersion::kV3) {
    return View{frame.data() + 8, frame.size() - 8 - 16};
  }
  return View{frame.data(), frame.size()};
}

void runMessageVector(const std::string& name, const Fields& f) {
  std::vector<uint8_t> msg;
  check(name == "discovery_msg" && parseHex(f.at("msg"), msg), name + ": msg hex parses");
  check(msg.size() == midea::kDiscoveryMessageLen, "discovery_msg: length matches");
  check(std::memcmp(msg.data(), midea::kDiscoveryMessage, msg.size()) == 0, "discovery_msg: bytes match const.py");
  check(std::stoul(f.at("port_1")) == midea::kDiscoveryPortPrimary, "discovery_msg: port 6445");
  check(std::stoul(f.at("port_2")) == midea::kDiscoveryPortSecondary, "discovery_msg: port 20086");
  check(std::stoul(f.at("default_packets")) == midea::kDiscoveryDefaultPackets, "discovery_msg: 3 packets default");
  check(f.at("broadcast") == midea::kDiscoveryBroadcast, "discovery_msg: broadcast address");
}

void runGoldenVector(const std::string& name, const Fields& f) {
  std::vector<uint8_t> frame;
  check(parseHex(f.at("frame"), frame), name + ": frame hex parses");
  const auto version = static_cast<midea::DiscoveryVersion>(std::stoi(f.at("version")));
  check(midea::getDiscoveryVersion(frame.data(), frame.size()) == version, name + ": version detect");

  // Intermediate encrypted slice and AES-ECB decrypt must match byte-exactly.
  const auto view = viewFor(frame, version);
  const size_t encLen = view.len - 56;
  const std::string encHex = toHex(view.data + 40, encLen);
  check(encHex == f.at("encrypted"), name + ": encrypted region slice");
  std::vector<uint8_t> enc;
  parseHex(encHex, enc);
  std::vector<uint8_t> dec(enc.size());
  size_t decLen = 0;
  const bool decOk = midea::decryptAesPkcs7(enc.data(), enc.size(), dec.data(), dec.size(), &decLen);
  check(decOk && toHex(dec.data(), decLen) == f.at("decrypted"), name + ": AES-ECB decrypt matches");

  midea::DiscoveryDeviceInfo info;
  const std::string ip = f.at("source_ip");
  const auto err = midea::parseDiscoveryResponse(frame.data(), frame.size(), version, ip.c_str(), &info);
  check(err == midea::DiscoveryError::kNone, name + ": parse ok");
  check(info.deviceId == std::strtoull(f.at("device_id").c_str(), nullptr, 10), name + ": device id");
  check(info.port == std::strtoul(f.at("port").c_str(), nullptr, 10), name + ": port");
  check(info.deviceType == static_cast<uint16_t>(std::strtoul(f.at("device_type").c_str(), nullptr, 16)),
        name + ": device type");
  check(info.version == version, name + ": version stored");
  check(std::strcmp(info.name, f.at("name").c_str()) == 0, name + ": name");
  check(std::strcmp(info.sn, f.at("sn").c_str()) == 0, name + ": sn");
  check(ip == info.ip, name + ": source ip reported");

  // Upstream reports the received IP even when the struct says otherwise.
  midea::DiscoveryDeviceInfo alt;
  check(midea::parseDiscoveryResponse(frame.data(), frame.size(), version, "192.168.5.5", &alt) ==
                midea::DiscoveryError::kNone &&
            std::strcmp(alt.ip, "192.168.5.5") == 0 && alt.deviceId == info.deviceId,
        name + ": mismatched source ip wins over embedded ip");
}

void runErrorVector(const std::string& name, const Fields& f) {
  std::vector<uint8_t> frame;
  check(parseHex(f.at("frame"), frame), name + ": frame hex parses");
  const auto version = midea::getDiscoveryVersion(frame.data(), frame.size());
  midea::DiscoveryDeviceInfo info;
  const auto err = midea::parseDiscoveryResponse(frame.data(), frame.size(), version, "10.0.0.1", &info);
  check(errorName(err) == f.at("error"), name + ": error is " + f.at("error") + " (got " + errorName(err) + ")");
}

void runVersionVectors(const std::string& name, const Fields& f) {
  std::vector<uint8_t> frame;
  check(parseHex(f.at("frame"), frame), name + ": frame hex parses");
  const auto want = static_cast<midea::DiscoveryVersion>(std::stoi(f.at("version")));
  check(midea::getDiscoveryVersion(frame.data(), frame.size()) == want, name + ": version " + f.at("version"));
}

void runEdgeCases() {
  std::printf("-- edge cases\n");
  midea::DiscoveryDeviceInfo info;
  const uint8_t v2[] = {0x5A, 0x5A};
  const uint8_t v3[] = {0x83, 0x70};
  check(midea::getDiscoveryVersion(nullptr, 0) == midea::DiscoveryVersion::kUnsupported, "edge: null data unsupported");
  check(midea::parseDiscoveryResponse(v2, sizeof(v2), midea::DiscoveryVersion::kUnsupported, "10.0.0.1", &info) ==
            midea::DiscoveryError::kTooShort,
        "edge: unsupported version rejected");
  check(midea::parseDiscoveryResponse(nullptr, 100, midea::DiscoveryVersion::kV2, "10.0.0.1", &info) ==
            midea::DiscoveryError::kTooShort,
        "edge: null data rejected");
  check(midea::parseDiscoveryResponse(v2, sizeof(v2), midea::DiscoveryVersion::kV2, "10.0.0.1", &info) ==
            midea::DiscoveryError::kTooShort,
        "edge: 2-byte v2 too short");
  const uint8_t mid[40] = {0x5A, 0x5A};
  check(midea::parseDiscoveryResponse(mid, sizeof(mid), midea::DiscoveryVersion::kV2, "10.0.0.1", &info) ==
            midea::DiscoveryError::kDecryptFailed,
        "edge: 40-byte v2 has no encrypted region");
  check(midea::parseDiscoveryResponse(v3, sizeof(v3), midea::DiscoveryVersion::kV3, "10.0.0.1", &info) ==
            midea::DiscoveryError::kTooShort,
        "edge: 2-byte v3 smaller than wrapper");
  const uint8_t v3small[30] = {0x83, 0x70};
  check(midea::parseDiscoveryResponse(v3small, sizeof(v3small), midea::DiscoveryVersion::kV3, "10.0.0.1", &info) ==
            midea::DiscoveryError::kTooShort,
        "edge: 30-byte v3 view < 26");
  // 42-byte view is exactly 56-14 short but block-aligned region exists at
  // len 72 (viewLen 72 -> encLen 16): decrypt must fail on content, not size.
  std::vector<uint8_t> v2aligned(72, 0);
  v2aligned[0] = 0x5A;
  v2aligned[1] = 0x5A;
  check(midea::parseDiscoveryResponse(v2aligned.data(), v2aligned.size(), midea::DiscoveryVersion::kV2, "10.0.0.1",
                                      &info) == midea::DiscoveryError::kDecryptFailed,
        "edge: block-aligned garbage fails decrypt");
}

} // namespace

int main() {
  const auto vectors = loadVectors("vectors/discovery.txt");
  if (vectors.empty()) return 1;
  for (const auto& [name, fields] : vectors) {
    if (fields.count("msg")) {
      runMessageVector(name, fields);
    } else if (fields.count("error")) {
      runErrorVector(name, fields);
    } else if (fields.count("device_id")) {
      runGoldenVector(name, fields);
    } else if (fields.count("version")) {
      runVersionVectors(name, fields);
    } else {
      check(false, name + ": unrecognized vector");
    }
  }
  runEdgeCases();

  if (gFailures != 0) {
    std::printf("%u checks FAILED\n", gFailures);
    return 1;
  }
  std::printf("discovery: all checks passed\n");
  return 0;
}
