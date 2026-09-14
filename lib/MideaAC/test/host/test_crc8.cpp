// Host test for midea::crc8 against golden vectors from msmart's
// crc8.calculate() (see vectors/NOTES.md for provenance).
#include "Crc8.h"

#include <cstdint>
#include <cstdio>
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

struct Vector {
  std::string input;
  std::string expected;
};

} // namespace

// Table + helper are constexpr: the byte_01 vector must also evaluate at
// compile time, proving flash placement and enabling constant folding.
constexpr uint8_t kByte01[] = {0x01};
static_assert(midea::crc8(kByte01, 1) == 0x5E);

int main() {
  std::ifstream file("vectors/crc8.txt");
  if (!file) {
    std::printf("FAIL: cannot open vectors/crc8.txt (run from test/host dir)\n");
    return 1;
  }

  std::vector<std::pair<std::string, Vector>> vectors;
  std::string name;
  Vector current;
  bool have = false;

  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue;
    if (line[0] != ' ') { // column-0 line = vector name
      if (have) vectors.emplace_back(name, current);
      name = line;
      current = Vector{};
      have = true;
      continue;
    }
    std::istringstream fields(line);
    std::string key, value;
    if (!(fields >> key >> value)) continue;
    if (key == "input") current.input = value;
    if (key == "expected") current.expected = value;
  }
  if (have) vectors.emplace_back(name, current);

  if (vectors.empty()) {
    std::printf("FAIL: no vectors parsed\n");
    return 1;
  }

  unsigned failures = 0;
  for (const auto& [vecName, vec] : vectors) {
    std::vector<uint8_t> data;
    std::vector<uint8_t> expected;
    if (!parseHex(vec.input, data) || !parseHex(vec.expected, expected) || expected.size() != 1) {
      std::printf("FAIL %-24s malformed vector\n", vecName.c_str());
      ++failures;
      continue;
    }
    const uint8_t got = midea::crc8(data.data(), data.size());
    if (got != expected[0]) {
      std::printf("FAIL %-24s crc8=%02x expected=%02x\n", vecName.c_str(), got, expected[0]);
      ++failures;
    } else {
      std::printf("ok   %-24s crc8=%02x\n", vecName.c_str(), got);
    }
  }

  if (failures != 0) {
    std::printf("%u/%zu vectors FAILED\n", failures, vectors.size());
    return 1;
  }
  std::printf("crc8: %zu vectors passed\n", vectors.size());
  return 0;
}
