// Host tests for the Open-Meteo response parser: real body decoding plus
// hostile inputs (redirect page, captive portal, truncation, short/long/
// ragged arrays, out-of-range values). The parser must never claim success
// on input that does not carry our fields.
#include "WeatherParser.h"

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                      \
      ++failures;                                                                                                      \
    }                                                                                                                  \
  } while (0)

using weather::kHourlyCount;
using weather::Snapshot;

bool parse(const char* body, Snapshot& s) {
  return weather::parseResponse(body, std::strlen(body), s);
}

// Captured live from the device (2026-09-07, Berlin request field set).
const char kRealBody[] =
    "{\"latitude\":52.52,\"longitude\":13.419998,\"generationtime_ms\":0.21708011627197266,"
    "\"utc_offset_seconds\":7200,\"timezone\":\"Europe/Berlin\","
    "\"timezone_abbreviation\":\"GMT+2\",\"elevation\":38.0,"
    "\"current_units\":{\"time\":\"iso8601\",\"interval\":\"seconds\",\"temperature_2m\":\"\u00b0C\","
    "\"relative_humidity_2m\":\"%\",\"apparent_temperature\":\"\u00b0C\","
    "\"weather_code\":\"wmo code\",\"wind_speed_10m\":\"km/h\"},"
    "\"current\":{\"time\":\"2026-09-07T22:15\",\"interval\":900,\"temperature_2m\":17.4,"
    "\"relative_humidity_2m\":74,\"apparent_temperature\":17.1,\"weather_code\":2,"
    "\"wind_speed_10m\":8.5},"
    "\"hourly_units\":{\"time\":\"iso8601\",\"temperature_2m\":\"\u00b0C\"},"
    "\"hourly\":{\"time\":[\"2026-09-07T22:00\",\"2026-09-07T23:00\",\"2026-09-08T00:00\","
    "\"2026-09-08T01:00\",\"2026-09-08T02:00\",\"2026-09-08T03:00\",\"2026-09-08T04:00\","
    "\"2026-09-08T05:00\",\"2026-09-08T06:00\",\"2026-09-08T07:00\",\"2026-09-08T08:00\","
    "\"2026-09-08T09:00\"],\"temperature_2m\":[17.6,16.9,16.3,16.1,15.9,15.7,15.5,15.6,16.0,"
    "16.4,17.6,19.2]}}";

void testRealBody() {
  Snapshot s;
  CHECK(parse(kRealBody, s));
  CHECK(s.hasTemp && s.tempC == 174);
  CHECK(s.hasHumidity && s.humidity == 74);
  CHECK(s.hasCode && s.code == 2);
  CHECK(s.hasApparent && s.apparentC == 171);
  CHECK(s.hasWind && s.windKmh == 85);
  CHECK(s.hasStartHour && s.startHour == 22);
  CHECK(s.hourlyCount == kHourlyCount);
  const int16_t expected[kHourlyCount] = {176, 169, 163, 161, 159, 157, 155, 156, 160, 164, 176, 192};
  for (uint8_t i = 0; i < kHourlyCount; ++i) {
    CHECK(s.hourlyTenths[i] == expected[i]);
  }
}

void testEmptyAndTiny() {
  Snapshot s;
  CHECK(!weather::parseResponse("", 0, s));
  CHECK(!parse("{}", s));
  CHECK(!parse("not json at all", s));
  CHECK(!s.hasTemp);
  CHECK(s.hourlyCount == 0);
}

void testRedirectAndCaptivePortal() {
  Snapshot s;
  // Plain redirect body.
  CHECK(!parse("<html><head><title>301 Moved Permanently</title></head>\n"
               "<body>301 Moved Permanently</body></html>",
               s));
  // Captive portal login page that even mentions our field names.
  CHECK(!parse("<html><body><h1>Login</h1><form><input name=\"temperature_2m\">"
               "<script>var x = \"temperature_2m\":; </script></body></html>",
               s));
  CHECK(!s.hasTemp);
  CHECK(s.hourlyCount == 0);
}

void testUnitsOnly() {
  Snapshot s;
  CHECK(!parse("{\"current_units\":{\"temperature_2m\":\"\u00b0C\",\"weather_code\":\"wmo code\"}}", s));
  CHECK(!s.hasTemp);
  CHECK(!s.hasCode);
}

void testNoTruncationTrust() {
  Snapshot s;
  // Truncated before any field: total failure.
  CHECK(!parse("{\"latitude\":52.52,\"gen", s));

  // Truncated inside the hourly array: current still lands, no curve.
  CHECK(parse("{\"current\":{\"temperature_2m\":21.3},\"hourly\":{\"temperature_2m\":[17.6,16.9", s));
  CHECK(s.hasTemp && s.tempC == 213);
  CHECK(s.hourlyCount == 0);

  // Truncated right after the 11th value: no curve (needs 12 + closing).
  CHECK(!parse("{\"hourly\":{\"temperature_2m\":[1,2,3,4,5,6,7,8,9,10,11", s));
}

void testHourlyShapeStrictness() {
  Snapshot s;
  // Short list.
  CHECK(!parse("{\"hourly\":{\"temperature_2m\":[17.6,16.9]}}", s));
  // Thirteen values: shape changed, refuse the curve.
  CHECK(!parse("{\"hourly\":{\"temperature_2m\":[1,2,3,4,5,6,7,8,9,10,11,12,13]}}", s));
  CHECK(s.hourlyCount == 0);
  // Ragged list (null hole).
  CHECK(!parse("{\"hourly\":{\"temperature_2m\":[17.6,null,16.3,16.1,15.9,15.7,15.5,15.6,16.0,16.4,17.6,19.2]}}", s));
  // Twelve values with whitespace/newlines (pretty-printed API).
  CHECK(parse("{\n \"hourly\": {\n  \"temperature_2m\" : [\n   17.6, 16.9, 16.3, 16.1, 15.9, 15.7, 15.5, 15.6,"
              " 16.0, 16.4, 17.6, 19.2\n  ]\n }\n}",
              s));
  CHECK(s.hourlyCount == kHourlyCount);
  CHECK(s.hourlyTenths[11] == 192);
}

void testValuesAndRanges() {
  Snapshot s;
  // Negative temperatures (hemisphere swap / winter).
  CHECK(parse("{\"current\":{\"temperature_2m\":-3.25},\"hourly\":{\"temperature_2m\":[-10.0,-10.5,-11.0,-11.5,"
              "-12.0,-12.5,-13.0,-13.5,-14.0,-14.5,-15.0,-15.5]}}",
              s));
  CHECK(s.hasTemp && s.tempC == -33); // half-up on magnitude
  CHECK(s.hourlyTenths[0] == -100);
  CHECK(s.hourlyTenths[11] == -155);

  // Integer form, no fraction.
  CHECK(parse("{\"current\":{\"temperature_2m\":17}}", s));
  CHECK(s.tempC == 170);

  // Out of physical range: rejected.
  CHECK(!parse("{\"current\":{\"temperature_2m\":1234.5}}", s));
  CHECK(!parse("{\"current\":{\"temperature_2m\":-978.3}}", s));
  CHECK(!parse("{\"current\":{\"relative_humidity_2m\":101}}", s));
  CHECK(!parse("{\"current\":{\"weather_code\":100}}", s));
  CHECK(!parse("{\"current\":{\"temperature_2m\":2.9e1}}", s)); // exponent rejected

  // Boundary code 99 and 0 accepted.
  CHECK(parse("{\"current\":{\"weather_code\":99,\"temperature_2m\":20.0}}", s));
  CHECK(s.hasCode && s.code == 99);
  CHECK(parse("{\"current\":{\"weather_code\":0,\"temperature_2m\":20.0}}", s));
  CHECK(s.hasCode && s.code == 0);
}

void testPartialSuccess() {
  // Hourly present but current missing: still a usable snapshot.
  Snapshot s;
  CHECK(parse("{\"hourly\":{\"temperature_2m\":[0,0,0,0,0,0,0,0,0,0,0,0]}}", s));
  CHECK(!s.hasTemp);
  CHECK(s.hourlyCount == kHourlyCount);

  // Current present, no hourly key at all.
  CHECK(parse("{\"current\":{\"temperature_2m\":20.0}}", s));
  CHECK(s.hasTemp && !s.hasHumidity && !s.hasCode && s.hourlyCount == 0);
}

void testHourlyStartTime() {
  Snapshot s;
  CHECK(parse("{\"hourly\":{\"time\": [ \"2026-01-01T07:00\",\"2026-01-01T08:00\" ],"
              "\"temperature_2m\":[-1.0,-1.5,-2.0,-2.5,-3.0,-3.5,-4.0,-4.5,-5.0,-5.5,-6.0,-6.5]}}",
              s));
  CHECK(s.hasStartHour && s.startHour == 7);

  Snapshot midnight;
  CHECK(parse("{\"hourly\":{\"time\":[\"2026-01-01T23:00\",\"2026-01-02T00:00\"],"
              "\"temperature_2m\":[1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0]}}",
              midnight));
  CHECK(midnight.hasStartHour && midnight.startHour == 23);

  Snapshot bad;
  CHECK(parse("{\"hourly\":{\"time\":[\"2026-01-01X25:00\"],"
              "\"temperature_2m\":[1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0]}}",
              bad));
  CHECK(!bad.hasStartHour);
  CHECK(bad.hourlyCount == kHourlyCount);
}

void testFlatWindowAndZeroes() {
  // All-equal hourly (flat forecast window — the sparkline edge case).
  Snapshot s;
  CHECK(parse("{\"hourly\":{\"temperature_2m\":[5.5,5.5,5.5,5.5,5.5,5.5,5.5,5.5,5.5,5.5,5.5,5.5]}}", s));
  for (uint8_t i = 0; i < kHourlyCount; ++i) {
    CHECK(s.hourlyTenths[i] == 55);
  }
}

void testCodeLabels() {
  CHECK(std::strcmp(weather::codeLabel(0), "Clear") == 0);
  CHECK(std::strcmp(weather::codeLabel(2), "Partly cloudy") == 0);
  CHECK(std::strcmp(weather::codeLabel(95), "Thunderstorm") == 0);
  CHECK(std::strcmp(weather::codeLabel(100), "??") == 0);
  CHECK(std::strcmp(weather::codeLabel(255), "??") == 0);
}

} // namespace

int main() {
  testRealBody();
  testEmptyAndTiny();
  testRedirectAndCaptivePortal();
  testUnitsOnly();
  testNoTruncationTrust();
  testHourlyShapeStrictness();
  testValuesAndRanges();
  testPartialSuccess();
  testHourlyStartTime();
  testFlatWindowAndZeroes();
  testCodeLabels();

  if (failures == 0) {
    std::printf("weatherparser: all checks passed\n");
    return 0;
  }
  std::printf("weatherparser: %d checks FAILED\n", failures);
  return 1;
}
