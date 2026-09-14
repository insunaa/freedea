// Host test for the Midea group-data (0xC1) parsers against golden vectors
// from midea-msmart TestGroupDataResponse (vectors/group.txt). The
// energy_binary vector's expectations are the *_binary decodes (Python
// test_binary_energy_usage asserts total_energy_binary etc.); every other
// energy vector asserts the BCD forms.
#include "Responses.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

static_assert(std::is_trivially_copyable<midea::AcExtStats>::value,
              "AcExtStats must stay trivially copyable for mutex snapshot copies");

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

void check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++gFailures;
}

void checkEq(const std::string& name, double got, const std::string& wantField) {
  const double want = std::strtod(wantField.c_str(), nullptr);
  check(std::fabs(got - want) < 1e-9, name + " = " + wantField);
}

void checkOpt(const std::string& name, const std::optional<double>& got, const std::string& wantField) {
  if (wantField == "none") {
    check(!got.has_value(), name + " is none");
    return;
  }
  if (!got.has_value()) {
    check(false, name + " set to " + wantField);
    return;
  }
  checkEq(name, *got, wantField);
}

void checkU8(const std::string& name, const std::optional<uint8_t>& got, int want) {
  check(got.has_value() && *got == static_cast<uint8_t>(want), name);
}

void checkU16(const std::string& name, const std::optional<uint16_t>& got, int want) {
  check(got.has_value() && *got == static_cast<uint16_t>(want), name);
}

bool startsWith(const std::string& s, const char* prefix) {
  return s.compare(0, std::strlen(prefix), prefix) == 0;
}

} // namespace

int main() {
  const auto vectors = loadVectors("vectors/group.txt");
  if (vectors.empty()) return 1;

  for (const auto& [name, fields] : vectors) {
    const bool isFrame = fields.count("frame") != 0;
    std::vector<uint8_t> bytes;
    if (!parseHex(fields.at(isFrame ? "frame" : "payload"), bytes)) {
      check(false, name + ": malformed hex");
      continue;
    }
    // Frame vectors carry the full frame; the parsers take frame[10:-2].
    if (isFrame && bytes.size() < 13) {
      check(false, name + ": frame too short");
      continue;
    }
    const uint8_t* payload = isFrame ? bytes.data() + 10 : bytes.data();
    const size_t len = isFrame ? bytes.size() - 12 : bytes.size();

    // The group byte is payload[3] & 0x0F; drive the umbrella dispatcher.
    const uint8_t group = payload[3] & 0x0F;
    midea::AcExtStats stats;
    if (!midea::parseGroupData(group, payload, len, stats)) {
      check(false, name + ": parseGroupData rejected the vector");
      continue;
    }
    check(true, name + ": parse accepted");

    if (startsWith(name, "energy_")) {
      // test_binary_energy_usage asserts the binary decodes; the rest BCD.
      const bool binary = name == "energy_binary";
      const auto& total = binary ? stats.totalEnergyKwhBinary : stats.totalEnergyKwhBcd;
      const auto& run = binary ? stats.runEnergyKwhBinary : stats.runEnergyKwhBcd;
      const auto& power = binary ? stats.realTimePowerWBinary : stats.realTimePowerWBcd;
      checkOpt(name + "/total", total, fields.at("expect_total"));
      checkOpt(name + "/current", run, fields.at("expect_current"));
      checkOpt(name + "/realtime", power, fields.at("expect_realtime"));
      if (fields.at("expect_total") == "none") {
        // The all-zero heuristic must leave the other form unset as well.
        check(!stats.totalEnergyKwhBinary.has_value() && !stats.realTimePowerWBcd.has_value(),
              name + ": both forms unset on all-zero");
      }
    } else if (name == "humidity" || name == "humidity_none") {
      const std::string want = fields.at("expect_humidity");
      if (want == "none") {
        check(!stats.humidityPercent.has_value(), name + ": humidity none");
      } else {
        checkU8(name + ": humidity", stats.humidityPercent, std::atoi(want.c_str()));
      }
    } else if (startsWith(name, "defrost_")) {
      const bool want = fields.at("expect_defrost") == "1";
      check(stats.defrost == want, name);
    } else if (name == "group1") {
      checkU8("group1/freq", stats.compressorFrequencyHz, std::atoi(fields.at("expect_freq").c_str()));
      checkU8("group1/target", stats.compressorTargetFrequencyHz, std::atoi(fields.at("expect_target_freq").c_str()));
      checkU8("group1/current", stats.compressorCurrent, std::atoi(fields.at("expect_current").c_str()));
      checkU8("group1/voltage", stats.compressorVoltageV, std::atoi(fields.at("expect_voltage").c_str()));
      checkEq("group1/T1", *stats.t1IndoorAmbientC, fields.at("expect_indoor"));
      checkEq("group1/T2", *stats.t2IndoorCoilC, fields.at("expect_indoor_coil"));
      checkEq("group1/T3", *stats.t3OutdoorCoilC, fields.at("expect_outdoor_coil"));
      checkEq("group1/T4", *stats.t4OutdoorAmbientC, fields.at("expect_outdoor"));
      checkU8("group1/TP", stats.dischargePipeC, std::atoi(fields.at("expect_tp").c_str()));
    } else if (name == "group2") {
      checkU16("group2/target_fan", stats.indoorFanTargetRpm, std::atoi(fields.at("expect_target_fan").c_str()));
      checkU16("group2/fan", stats.indoorFanRpm, std::atoi(fields.at("expect_fan").c_str()));
      check(stats.waterPumpRunning == (fields.at("expect_pump") == "1"), "group2/pump");
    } else if (name == "group7") {
      checkU16("group7/power", stats.outdoorUnitPowerW, std::atoi(fields.at("expect_power").c_str()));
    } else if (name == "group11") {
      checkU8("group11/h", stats.horizontalLouverDeg, std::atoi(fields.at("expect_h_louvers").c_str()));
      checkU8("group11/v", stats.verticalLouverDeg, std::atoi(fields.at("expect_v_louvers").c_str()));
    } else {
      check(false, name + ": no handler for this vector");
    }
  }

  // Accumulation: later groups must not clobber earlier ones.
  {
    const uint8_t g1[] = {0xc1, 0x00, 0x00, 0x41, 0x1c, 0x1d, 0x00, 0x01, 0xe8, 0x00,
                          0x47, 0x26, 0x66, 0x58, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x00};
    const uint8_t g7[] = {0xc1, 0x00, 0x00, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x0d, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    midea::AcExtStats stats;
    check(midea::parseGroupData(1, g1, sizeof(g1), stats), "accumulate: g1 accepted");
    check(!stats.outdoorUnitPowerW.has_value(), "accumulate: g7 field empty before g7");
    check(midea::parseGroupData(7, g7, sizeof(g7), stats), "accumulate: g7 accepted");
    checkU8("accumulate: g1 freq survives", stats.compressorFrequencyHz, 28);
    checkU16("accumulate: g7 power", stats.outdoorUnitPowerW, 269);
  }

  // Rejection paths: unknown group, short payload, null payload.
  {
    const uint8_t g5[] = {0xc1, 0x21, 0x01, 0x45, 0x3f, 0x54, 0x6c, 0x00, 0x5d, 0x0a, 0x00};
    midea::AcExtStats stats;
    check(!midea::parseGroupData(3, g5, sizeof(g5), stats), "unknown group 3 rejected");
    check(!midea::parseGroupData(5, g5, sizeof(g5) - 1, stats), "short g5 payload rejected");
    check(!midea::parseGroupData(5, nullptr, sizeof(g5), stats), "null payload rejected");
    check(!midea::parseGroupData(1, g5, sizeof(g5), stats), "wrong-length payload for group 1 rejected");
    // A rejected parse must not have written anything.
    check(!stats.humidityPercent.has_value() && !stats.compressorFrequencyHz.has_value(),
          "rejected parses leave the snapshot untouched");
  }

  if (gFailures != 0) {
    std::printf("%u checks FAILED\n", gFailures);
    return 1;
  }
  std::printf("groups: all checks passed\n");
  return 0;
}
