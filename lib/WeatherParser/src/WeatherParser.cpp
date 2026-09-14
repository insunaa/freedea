#include "WeatherParser.h"

#include <cstring>

namespace weather {

namespace {

// Bounds-checked substring search over the (possibly unterminated) body.
const char* findSeq(const char* hay, size_t hayLen, const char* needle, size_t needleLen) {
  if (needleLen > hayLen) return nullptr;
  const char first = needle[0];
  for (size_t i = 0; i + needleLen <= hayLen; ++i) {
    if (hay[i] == first && std::memcmp(hay + i, needle, needleLen) == 0) {
      return hay + i;
    }
  }
  return nullptr;
}

bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isDigit(char c) {
  return c >= '0' && c <= '9';
}

const char* skipSpace(const char* p, const char* end) {
  while (p < end && isSpace(*p))
    ++p;
  return p;
}

// Parses a JSON number (optional sign, integer part, optional fraction) at
// `p` and returns the value in tenths with half-up rounding. Rejects values
// outside [lo, hi] and anything exponent- or digit-count-suspicious, so a
// non-weather body can never forge a field. Returns the end pointer or
// nullptr when `p` does not start with such a number.
const char* scanTenths(const char* p, const char* end, int16_t lo, int16_t hi, int16_t& out) {
  if (p >= end) return nullptr;
  bool negative = false;
  if (*p == '-') {
    negative = true;
    ++p;
  } else if (*p == '+') {
    ++p;
  }
  const char* digitsStart = p;
  int32_t whole = 0;
  uint8_t digitCount = 0;
  while (p < end && *p >= '0' && *p <= '9' && digitCount < 5) {
    whole = whole * 10 + (*p - '0');
    ++p;
    ++digitCount;
  }
  if (digitCount == 0) return nullptr;
  // Reject a stuck exponent (1.2e3) or trailing digits: this is not a weather
  // value we can trust, and 5 integer digits already exceeds the ranges.
  if (p < end && (*p == 'e' || *p == 'E')) return nullptr;
  if (p < end && *p == '.') {
    ++p;
    if (p >= end || *p < '0' || *p > '9') return nullptr;
    const int32_t frac = *p - '0';
    ++p;
    // Round on the second fraction digit and demand a real separator after
    // the number otherwise.
    int32_t next = 0;
    if (p < end && *p >= '0' && *p <= '9') {
      next = *p - '0';
      ++p;
      while (p < end && *p >= '0' && *p <= '9')
        ++p; // tolerate 25.50
    }
    whole = whole * 10 + frac + (next >= 5 ? 1 : 0);
  } else {
    whole *= 10;
  }
  if (p < end && (*p == 'e' || *p == 'E')) return nullptr;
  const int32_t value = negative ? -whole : whole;
  if (value < lo || value > hi) return nullptr;
  out = static_cast<int16_t>(value);
  return p;
}

// Parses an unsigned integer at `p` within [lo, hi]; returns end or nullptr.
const char* scanU8(const char* p, const char* end, uint8_t lo, uint8_t hi, uint8_t& out) {
  if (p >= end || *p < '0' || *p > '9') return nullptr;
  uint16_t value = 0;
  uint8_t digits = 0;
  while (p < end && *p >= '0' && *p <= '9' && digits < 4) {
    value = static_cast<uint16_t>(value * 10 + (*p - '0'));
    ++p;
    ++digits;
  }
  if (value < lo || value > hi) return nullptr;
  out = static_cast<uint8_t>(value);
  return p;
}

// Value ranges (in the units above). Generous physical-world bounds: they
// exist to reject misreads, not to validate the forecast.
constexpr int16_t kTempLo = -900; // -90.0 C
constexpr int16_t kTempHi = 600;  // 60.0 C
constexpr int16_t kWindHi = 6000; // 600.0 km/h

// Locates `"key":` (needle includes key quotes + colon) at or after `from`
// and returns the position of the value (whitespace skipped), or nullptr.
// String values (the *_units objects) are skipped by the caller's scan loop
// when the value parser rejects them.
const char* findKeyValue(const char* body, size_t len, const char* key, const char* from) {
  const size_t keyLen = std::strlen(key);
  const char* searchFrom = from ? from : body;
  while (searchFrom < body + len) {
    const size_t tail = static_cast<size_t>(body + len - searchFrom);
    const char* hit = findSeq(searchFrom, tail, key, keyLen);
    if (hit == nullptr) return nullptr;
    const char* value = skipSpace(hit + keyLen, body + len);
    // The needle ends with ':', so `value` points at the first byte after it;
    // a quote means a unit string — keep searching for a numeric occurrence.
    if (value < body + len && *value != '"') {
      return value;
    }
    searchFrom = hit + 1;
  }
  return nullptr;
}

} // namespace

bool parseResponse(const char* body, size_t len, Snapshot& out) {
  out = Snapshot{};
  if (body == nullptr || len < 32) return false; // shorter than any useful response
  const char* end = body + len;

  // Current block: the scalar occurrences of each key (the _units objects
  // carry string values and are skipped).
  if (const char* p = findKeyValue(body, len, "\"temperature_2m\":", nullptr)) {
    const char* after = scanTenths(p, end, kTempLo, kTempHi, out.tempC);
    if (after != nullptr) out.hasTemp = true;
  }
  if (const char* p = findKeyValue(body, len, "\"apparent_temperature\":", nullptr)) {
    const char* after = scanTenths(p, end, kTempLo, kTempHi, out.apparentC);
    if (after != nullptr) out.hasApparent = true;
  }
  if (const char* p = findKeyValue(body, len, "\"relative_humidity_2m\":", nullptr)) {
    const char* after = scanU8(p, end, 0, 100, out.humidity);
    if (after != nullptr) out.hasHumidity = true;
  }
  if (const char* p = findKeyValue(body, len, "\"weather_code\":", nullptr)) {
    const char* after = scanU8(p, end, 0, 99, out.code);
    if (after != nullptr) out.hasCode = true;
  }
  if (const char* p = findKeyValue(body, len, "\"wind_speed_10m\":", nullptr)) {
    int16_t value = 0;
    if (scanTenths(p, end, 0, kWindHi, value) != nullptr) {
      out.windKmh = static_cast<uint16_t>(value);
      out.hasWind = true;
    }
  }

  // Hourly temperature array: locate `"temperature_2m"` followed by `[`
  // (scalars and unit strings do not match) and scan exactly 12 numbers
  // terminated by `]` — a short, ragged or oversized list is "no forecast",
  // never a partial curve.
  {
    const char* needle = "\"temperature_2m\"";
    const size_t needleLen = std::strlen(needle);
    const char* search = body;
    while (search < end) {
      const size_t tail = static_cast<size_t>(end - search);
      const char* hit = findSeq(search, tail, needle, needleLen);
      if (hit == nullptr) break;
      const char* p = skipSpace(hit + needleLen, end);
      if (p >= end || *p != ':') {
        search = hit + 1;
        continue;
      }
      p = skipSpace(p + 1, end);
      if (p >= end || *p != '[') {
        search = hit + 1;
        continue;
      }
      ++p; // past '['
      // Parse values until the list closes or stops looking like numbers;
      // one extra iteration detects an oversized (shape-changed) list.
      int16_t values[kHourlyCount];
      uint8_t count = 0;
      bool closed = false;
      while (count <= kHourlyCount) {
        p = skipSpace(p, end);
        int16_t value = 0;
        const char* after = scanTenths(p, end, kTempLo, kTempHi, value);
        if (after == nullptr) break;
        if (count < kHourlyCount) values[count] = value;
        ++count;
        p = skipSpace(after, end);
        if (p < end && *p == ',') {
          ++p;
          continue;
        }
        if (p < end && *p == ']') {
          closed = true;
          break;
        }
        break; // any other byte: not a clean number list
      }
      if (closed && count == kHourlyCount) {
        out.hourlyCount = kHourlyCount;
        for (uint8_t i = 0; i < kHourlyCount; ++i) {
          out.hourlyTenths[i] = values[i];
        }
      }
      // Only the hourly object can match the '[' shape, so stop either way.
      break;
    }
  }

  // Hourly start hour: the hourly.time array carries the local timestamp of
  // every hourly point. Only the first hour is needed for the graph's X axis,
  // and timezone=auto makes it local to the configured coordinates.
  {
    const char* needle = "\"time\":";
    const size_t needleLen = std::strlen(needle);
    const char* search = body;
    while (search < end) {
      const size_t tail = static_cast<size_t>(end - search);
      const char* hit = findSeq(search, tail, needle, needleLen);
      if (hit == nullptr) break;

      const char* p = skipSpace(hit + needleLen, end);
      if (p >= end || *p != '[') {
        search = hit + 1;
        continue;
      }
      p = skipSpace(p + 1, end);
      if (p >= end || *p != '"') {
        search = hit + 1;
        continue;
      }
      ++p;
      // First array item: "YYYY-MM-DDTHH:mm" (or a longer suffix). Accept the
      // hour only when the fixed date/time separators line up and the hour is
      // in range; otherwise the forecast times are unusable for labeling.
      if (end - p >= 13 && p[4] == '-' && p[7] == '-' && p[10] == 'T' && isDigit(p[11]) && isDigit(p[12])) {
        const uint8_t hour = static_cast<uint8_t>((p[11] - '0') * 10 + (p[12] - '0'));
        if (hour < 24) {
          out.hasStartHour = true;
          out.startHour = hour;
        }
      }
      break;
    }
  }

  return out.hasTemp || out.hourlyCount == kHourlyCount;
}

const char* codeLabel(uint8_t code) {
  struct Entry {
    uint8_t code;
    const char* label;
  };
  static constexpr Entry kLabels[] = {
      {0, "Clear"},          {1, "Mainly clear"},   {2, "Partly cloudy"},  {3, "Overcast"},       {45, "Fog"},
      {48, "Rime fog"},      {51, "Light drizzle"}, {53, "Drizzle"},       {55, "Dense drizzle"}, {56, "Frz drizzle"},
      {57, "Frz drizzle"},   {61, "Light rain"},    {63, "Rain"},          {65, "Heavy rain"},    {66, "Frz rain"},
      {67, "Frz rain"},      {71, "Light snow"},    {73, "Snow"},          {75, "Heavy snow"},    {77, "Snow grains"},
      {80, "Light showers"}, {81, "Showers"},       {82, "Violent shwrs"}, {85, "Snow showers"},  {86, "Snow showers"},
      {95, "Thunderstorm"},  {96, "T-storm/hail"},  {99, "T-storm/hail"},
  };
  for (const Entry& e : kLabels) {
    if (e.code == code) return e.label;
  }
  return "??";
}

} // namespace weather
