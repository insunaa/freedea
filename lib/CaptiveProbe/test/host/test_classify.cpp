// Host tests for the captive-probe classifier (7.2c): the exact empty 204 is
// the only "open"; every other HTTP response classifies as a portal.

#include <CaptiveProbe.h>

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

} // namespace

int main() {
  using captive::classify;

  // The canonical unhindered-network reply.
  check(classify(204, 0) == captive::kOpen, "empty 204 is open");

  // Interception in its observed shapes: 3xx redirect to the portal, the
  // portal's own 200 page (with body), an empty 200, a 404 captive page, and
  // the RFC-specified 511 Network Authentication Required.
  check(classify(301, 0) == captive::kPortal, "301 is portal");
  check(classify(302, 743) == captive::kPortal, "302 with body is portal");
  check(classify(200, 1200) == captive::kPortal, "200 page is portal");
  check(classify(200, 0) == captive::kPortal, "empty 200 is portal");
  check(classify(404, 512) == captive::kPortal, "404 is portal");
  check(classify(511, 300) == captive::kPortal, "511 is portal");

  // A 204 that carries bytes is not the canonical reply either.
  check(classify(204, 1) == captive::kPortal, "204 with body is portal");

  // Verdict values must match the settings cache encoding so the firmware can
  // store them without mapping (kept in sync by hand; the firmware mirrors
  // this with a static_assert).
  check(captive::kOpen == 1 && captive::kPortal == 2, "verdict values match kPortalOpen/kPortalFound");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all classifier checks passed\n");
  return EXIT_SUCCESS;
}
