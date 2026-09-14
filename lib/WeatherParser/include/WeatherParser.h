#pragma once

// Open-Meteo response parser (6.2b). Pure C++ (no Arduino/JSON library): the
// body arrives as raw bytes over HTTPClient and is scanned, never trusted.
// A redirect page, captive-portal login, error page or truncated response
// parses to nothing and reports failure — callers keep their last good
// snapshot. Field values are individually range-checked; the hourly block
// must be a complete 12-value list or counts as absent.

#include <cstddef>
#include <cstdint>

namespace weather {

constexpr uint8_t kHourlyCount = 12;

// Snapshot of one successful parse. Only individually parsed-and-in-range
// fields carry values. Trivially copyable (~40 B) for publishing across tasks.
struct Snapshot {
  bool hasTemp = false;
  bool hasApparent = false;
  bool hasHumidity = false;
  bool hasCode = false;
  bool hasWind = false;
  bool hasStartHour = false;
  int16_t tempC = 0;     // degrees C, 0.1 units
  int16_t apparentC = 0; // degrees C, 0.1 units
  uint8_t humidity = 0;  // %RH
  uint8_t code = 0;      // WMO weather interpretation code
  uint16_t windKmh = 0;  // km/h, 0.1 units
  // Exactly kHourlyCount entries after a complete hourly block, else 0.
  uint8_t hourlyCount = 0;
  // Local hour (0..23) of hourlyTenths[0] when hourly.time parsed. Valid only
  // when hasStartHour is true.
  uint8_t startHour = 0;
  int16_t hourlyTenths[kHourlyCount] = {}; // degrees C, 0.1 units
};

// Scans an Open-Meteo forecast body for the current block and the
// `hourly.temperature_2m` array (the request field set used by the firmware).
// `out` is reset first. Returns true when at least the current temperature
// or a full 12-value hourly block parsed, so a body that is merely missing
// one field still updates the rest.
bool parseResponse(const char* body, size_t len, Snapshot& out);

// Short ASCII label for a WMO weather code (flash-resident table), "??" for
// unknown codes.
const char* codeLabel(uint8_t code);

} // namespace weather
