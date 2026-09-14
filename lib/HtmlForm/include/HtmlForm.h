#pragma once

// First-<form> HTML extraction and login-request building for the captive-
// portal auto-login (7.2d). Pure C++ (no Arduino, host-tested under
// test/host/); the fetch side lives in src/service/PortalLoginService.
//
// Deliberately a tolerant single-purpose parser, not an HTML engine: it reads
// the FIRST <form> of a page, applies fixed auto-login fill rules, and
// builds the one urlencoded request the portal login needs. Malformed
// markup is skipped field-by-field; HTML comments and <select> elements are
// not handled (portals of the "tick a box, press go" class do not need them).

#include <cstddef>
#include <cstdint>

namespace htmlform {

// Field caps: real portals carry few fields; the bounds fail cleanly rather
// than grow the firmware's static footprint. An over-long name drops the
// field; an over-long value is truncated (a broken token degrades to the
// caller's "could not join" path).
constexpr size_t kMaxFields = 12;
constexpr size_t kMaxFieldName = 32;
constexpr size_t kMaxFieldValue = 128;
constexpr size_t kMaxActionLen = 192;
constexpr size_t kMaxUrl = 512;
constexpr size_t kMaxBody = 2048;

struct Field {
  char name[kMaxFieldName + 1];
  char value[kMaxFieldValue + 1];
};

struct Form {
  char action[kMaxActionLen + 1]; // raw attribute, "" when absent
  bool post;
  uint8_t fieldCount;
  Field fields[kMaxFields];
};

// Parses the first <form ...>...</form> (no NUL termination required). Fill
// rules: checkboxes are always checked (value attribute, default "on");
// radios are submitted when the document marks one checked; hidden fields
// keep their value attribute; text-like inputs and textareas contribute only
// their pre-filled (non-empty) values; submit buttons contribute
// name=value (input default "on", <button> falls back to its trimmed text
// content). Disabled fields, reset/file/button inputs and selects are
// skipped. Returns false only when the page contains no <form> at all;
// surplus fields past kMaxFields are dropped.
bool parseFirstForm(const char* html, size_t len, Form& out);

// Resolves a form action or redirect Location against the page URL with the
// usual rules (absolute http URLs pass through; "//host" gains http:; "/path"
// is host-relative; "rel" replaces the last path segment; "?q" keeps the page
// path; fragments are dropped). Non-http schemes (https included) and
// malformed page URLs return false, as does overflow.
bool resolveUrl(const char* target, const char* pageUrl, char* out, size_t outCap);

// Builds the login request for a parsed form served at pageUrl:
//   GET:  url = resolved action without its query + urlencoded pairs,
//         body stays empty (browsers replace the query on GET submits)
//   POST: url = resolved action (its query kept), body = urlencoded pairs
// Empty action targets the page URL itself. A non-http action fails without
// producing a request. Pairs keep document order and encode like
// application/x-www-form-urlencoded (space as '+', uppercase %XX).
// Returns false for https/other schemes, malformed URLs or overflow.
bool buildRequest(const Form& form, const char* pageUrl, char* url, size_t urlCap, char* body, size_t bodyCap);

// Percent-encodes one string (unreserved set A-Za-z0-9-_.~ and '*', space as
// '+', everything else uppercase %XX). Returns false when it does not fit.
bool encodeValue(const char* value, char* out, size_t cap);

} // namespace htmlform
