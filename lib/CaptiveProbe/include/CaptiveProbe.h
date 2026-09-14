#pragma once

// Captive-portal probe classification (7.2c). Pure C++ and host-tested under
// test/host/; the Arduino side (task + HTTPClient) lives in the firmware's
// src/service/CaptiveProbeService.
//
// The probe is a single GET against a connectivity-check URL: an unhindered
// network answers with an empty HTTP 204; anything else (3xx, the portal's
// own 200 interception page, foreign bodies) is evidence of a captive
// portal. Connect/DNS failures are not a verdict at all — the caller caches
// nothing and re-probes on the next connect.

#include <cstddef>
#include <cstdint>

namespace captive {

// Values deliberately equal settings::kPortalOpen / settings::kPortalFound so
// the verdict stores into the profile as-is (static_assert'd in the service).
enum Verdict : uint8_t {
  kOpen = 1,
  kPortal = 2,
};

// Connectivity-check URL: plain HTTP 204 when egress works, no TLS needed.
// Captive DNS usually resolves it to the interception page instead.
constexpr const char* kProbeUrl = "http://clients3.google.com/generate_204";

// Pure classifier: only the exact empty 204 counts as open. Reached only with
// an HTTP response (status > 0); transport failures never get here.
constexpr Verdict classify(int httpStatus, size_t bodyBytes) {
  return (httpStatus == 204 && bodyBytes == 0) ? kOpen : kPortal;
}

} // namespace captive
