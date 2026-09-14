#include "WebPortal.h"

#include <Arduino.h>

#include <Devices.h>
#include <Settings.h>

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "DeviceStore.h"
#include "SettingsStore.h"

namespace {

constexpr const char* kContentType = "text/html";

// Page markup: one raw-string constant per fragment (flash .rodata), composed
// with snprintf in the handlers. Multi-line raw strings keep the markup
// readable; the literal whitespace ships to the browser harmlessly.

// WiFi network list (7.2b): one self-contained POST /net form per saved
// profile, plus an add form targeting the first free slot. The three submit
// buttons carry name="action" — the clicked button's value (save/connect/
// delete) is the verb. The active profile shows an "active" marker instead
// of a Connect button; blank password keeps the stored one (on a new profile
// it stays empty = open network). Passwords are never rendered back.
const char kNetHead[] = R"HTML(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Freedea</title>
</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em">
<h1>Freedea</h1>
<h2>WiFi networks</h2>
<p><small>Up to 4 saved networks. Changing which network is active restarts
the device (a few seconds); edits to a network that is not active apply
immediately. Switch networks on the device under Settings &rarr; WiFi.</small></p>
)HTML";

// Row: <form> + SSID value + password/hidden-slot inputs + slot digit +
// action-button tail (active variant omits Connect, adds the marker).
constexpr const char kNetRowHead[] =
    "<form method=\"post\" action=\"/net\"><p><label>Network (SSID)<br><input name=\"ssid\" maxlength=\"32\" value=\"";
constexpr const char kNetRowMid[] = "\"></label> <label>Password <input type=\"password\" name=\"password\" "
                                    "maxlength=\"63\" placeholder=\"blank keeps current\"> "
                                    "<input type=\"hidden\" name=\"slot\" value=\"";
constexpr const char kNetRowTailNormal[] = "\"><p><button name=\"action\" value=\"save\">Save</button> <button "
                                           "name=\"action\" value=\"connect\">Connect</button> "
                                           "<button name=\"action\" value=\"delete\" onclick=\"return confirm('Remove "
                                           "this network from Freedea?')\">Remove</button></p></form>";
constexpr const char kNetRowTailActive[] = "\"><p><button name=\"action\" value=\"save\">Save</button> "
                                           "<button name=\"action\" value=\"delete\" onclick=\"return confirm('Remove "
                                           "this network from Freedea?')\">Remove</button> "
                                           "<b>active</b></p></form>";

// Cached connectivity verdict (7.2c) rendered under the profile's form;
// never-probed profiles render nothing.
constexpr const char kNetStateOpen[] = "<p><small>Last check: internet works (no login page)</small></p>";
constexpr const char kNetStatePortal[] = "<p><small>Last check: login page found</small></p>";

// Add form for the first free slot; %u is the slot index. With the store
// full it becomes the kNetFullNote instead.
const char kNetAddForm[] = R"HTML(
<h2>Add a network</h2>
<form method="post" action="/net">
<p><label>Network (SSID)<br><input name="ssid" maxlength="32" placeholder="required"></label></p>
<p><label>Password <input type="password" name="password" maxlength="63" placeholder="blank = open network"></label></p>
<input type="hidden" name="slot" value="%u">
<p><button name="action" value="save">Add network</button></p>
</form>
)HTML";

constexpr const char kNetFullNote[] = "<p><small>All 4 network slots are used - remove one to add another.</small></p>";
constexpr const char kDevicesHead[] = "<h2>Midea devices</h2>\n";

// Device add/update form. id accepts decimal or 0x; token+key are only used
// together for V3 devices and are never echoed back. Blank token/key/IP keep
// the stored values when editing an existing id (same contract as the WiFi
// password field); a new id gets no IP until it is filled in here or by
// discovery (Phase 4.4).
const char kDeviceForm[] = R"HTML(
<form method="post" action="/device">
<p><label>Device id (decimal or 0x...)<br>
<input name="id" maxlength="20" style="width:96%" placeholder="required"></label></p>
<p><label>Name<br>
<input name="name" maxlength="23" style="width:96%" placeholder="optional"></label></p>
<p><label>IP address<br>
<input name="ip" maxlength="15" style="width:96%" placeholder="leave blank to keep current"></label></p>
<p><label>V3 token (hex)<br>
<input name="token" maxlength="256" style="width:96%" placeholder="leave blank to keep current"></label></p>
<p><label>V3 key (64 hex chars)<br>
<input name="key" maxlength="64" style="width:96%" placeholder="leave blank to keep current"></label></p>
<p><button type="submit">Save device</button></p>
</form>
)HTML";

// Weather settings form (6.2a): one POST /weather, applied hot (no reboot —
// the weather task picks settings up on its next cycle, unlike credentials
// which need a radio restart). Placeholders: " checked", escaped name, and
// the stored lat/lon as decimal strings (empty when unset).
const char kWeatherForm[] = R"HTML(
<h2>Weather (Open-Meteo)</h2>
<p><small>Fetched over HTTP every 30 minutes while powered. Applied immediately; no reboot.</small></p>
<form method="post" action="/weather">
<p><label><input type="checkbox" name="en" value="1"%s> Enable weather panel</label></p>
<p><label>Location name<br>
<input name="name" maxlength="16" style="width:96%" placeholder="optional, e.g. Berlin" value="%s"></label></p>
<p><label>Latitude (-90..90)<br>
<input name="lat" maxlength="15" style="width:96%" inputmode="decimal" placeholder="required to enable" value="%s"></label></p>
<p><label>Longitude (-180..180)<br>
<input name="lon" maxlength="16" style="width:96%" inputmode="decimal" placeholder="required to enable" value="%s"></label></p>
<p><button type="submit">Save weather</button></p>
</form>
)HTML";

// WireGuard settings form (7.3): one POST /wireguard, applied hot (no reboot
// — the service diffs the config on its main-loop tick). Keys are never
// rendered back: blank means keep the stored one. Placeholders: " checked",
// escaped endpoint, port, tunnel IP, keepalive.
const char kWireGuardForm[] = R"HTML(
<h2>WireGuard</h2>
<p><small>Optional VPN client for remote access. While the tunnel is up, ALL
text traffic from this device routes through the WireGuard server, so the
server must route or NAT whatever the device needs. Keys are stored on the SD
card like every other credential here.</small></p>
<form method="post" action="/wireguard">
<p><label><input type="checkbox" name="en" value="1"%s> Enable WireGuard</label></p>
<p><label>Server endpoint (hostname or IPv4)<br>
<input name="ep" maxlength="64" style="width:96%%" placeholder="wg.example.org (blank keeps stored)" value="%s"></label></p>
<p><label>UDP port<br>
<input name="port" maxlength="5" style="width:96%%" inputmode="numeric" placeholder="51820" value="%s"></label></p>
<p><label>Own private key (44 chars, base64)<br>
<input name="priv" maxlength="44" style="width:96%%" autocomplete="off" placeholder="blank keeps stored"></label></p>
<p><label>Peer public key (44 chars, base64)<br>
<input name="pub" maxlength="44" style="width:96%%" autocomplete="off" placeholder="blank keeps stored"></label></p>
<p><label>Own tunnel IP<br>
<input name="tip" maxlength="15" style="width:96%%" inputmode="numeric" placeholder="10.66.66.2" value="%s"></label></p>
<p><label>Keepalive seconds (0 = default 10, max 300)<br>
<input name="ka" maxlength="3" style="width:96%%" inputmode="numeric" placeholder="25" value="%s"></label></p>
<p><button type="submit">Save WireGuard</button></p>
</form>
)HTML";

const char kNoneStored[] = "<p>none stored yet</p>";

const char kFooter[] = R"HTML(
<p><small>WiFi credentials and devices are saved encrypted to the SD card
(/.freedea/) with a key derived from this device's chip. A hand-editable
settings.json or device.json on the card is imported on boot and then
removed. The portal has no authentication: use it on a trusted LAN
only.</small></p>
</body></html>
)HTML";

const char kPageHead[] = R"HTML(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
)HTML";

// Reboot-to-apply: this page is the last thing the old link ever sees, so it
// carries no refresh or back link.
const char kPageSaved[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Saved. The device is restarting with the new credentials (a few seconds).</p></body></html>)HTML";

const char kPageDeviceSaved[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Device saved. The device is restarting (a few seconds).</p></body></html>)HTML";

const char kPageDeviceRemoved[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Device removed. The device is restarting (a few seconds).</p></body></html>)HTML";

const char kPageNotPersisted[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Applied, but the SD write failed: these changes are lost on reboot.</p><p><a href="/">back</a></body></html>)HTML";

const char kPageUnchanged[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>No changes (a blank password keeps the stored one).</p><p><a href="/">back</a></body></html>)HTML";

const char kPageNetworkInvalid[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Invalid input: SSID is required (up to 32 characters); password either blank (open network, or keep the stored one) or 8-63 characters.</p><p><a href="/">back</a></body></html>)HTML";

// Radio-affecting network change (active profile's credentials, connect,
// removing the active profile, first saved network): the reboot is the apply.
const char kPageNetworkReboot[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Saved. The device is restarting and will connect with the updated networks (a few seconds).</p></body></html>)HTML";

// Non-radio network change: the store is updated, the link untouched.
const char kPageNetworkSaved[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Network saved. It is not active; use its Connect button or Settings &rarr; WiFi on the device to switch.</p><p><a href="/">back</a></body></html>)HTML";

const char kPageNetworkRemoved[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Network removed.</p><p><a href="/">back</a></body></html>)HTML";

const char kPageNetworkActive[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>That network is already active.</p><p><a href="/">back</a></body></html>)HTML";

const char kPageDeviceInvalid[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Invalid device: id is required (decimal or 0x, non-zero); name up to 23 characters; IP is optional and must be dotted-quad; V3 token and key (64 hex chars) must be entered together, or both blank to keep the stored credentials (none for a new unauthenticated device).</p><p><a href="/">back</a></body></html>)HTML";

const char kPageDeviceFull[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Device store is full (4 devices). Delete /.freedea/devices.bin on the SD card and reboot to start over.</p><p><a href="/">back</a></body></html>)HTML";

const char kPageNotFound[] =
    R"HTML(</head><body style="font-family:sans-serif"><h1>Freedea</h1><p>Not found. <a href="/">back</a></body></html>)HTML";

const char kPageWeatherSaved[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Weather settings saved. The Dashboard panel updates within a poll cycle (or use Settings -> Weather on the device).</p><p><a href="/">back</a></body></html>)HTML";

const char kPageWeatherInvalid[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Invalid weather settings: name up to 16 characters; latitude -90..90 and longitude -180..180 (blank keeps the stored value); enabling weather requires a location.</p><p><a href="/">back</a></body></html>)HTML";

const char kPageWireGuardSaved[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>WireGuard settings saved. The tunnel (re)connects automatically; Settings &rarr; WireGuard on the device shows its state.</p><p><a href="/">back</a></body></html>)HTML";

const char kPageWireGuardInvalid[] =
    R"HTML(</head><body style="font-family:sans-serif;max-width:480px;margin:2em auto;padding:0 1em"><h1>Freedea</h1><p>Invalid WireGuard settings: endpoint up to 64 characters; port 1-65535; keys are exactly 44 characters (blank keeps the stored one); tunnel IP must be a dotted quad; keepalive 0-300. Enabling requires endpoint, port, both keys and a tunnel IP.</p><p><a href="/">back</a></body></html>)HTML";

// Each device row embeds a remove-button form: <li>name <small>id N</small>
// <form POST /remove (browser-confirmed)>hidden id N, remove button</form>. All
// static markup; the id appears twice (label + hidden field), sized for
// kDeviceRowMax.
constexpr const char kRowIdHead[] = " <small>id ";
constexpr const char kRowRemoveHead[] =
    "</small> <form style=\"display:inline\" method=\"POST\" action=\"/remove\" onsubmit=\"return confirm('Remove this "
    "device from Freedea?')\"><input type=\"hidden\" name=\"id\" value=\"";
constexpr const char kRowRemoveTail[] = "\"><button type=\"submit\">remove</button></form></li>";

// Worst case one stored-device row: <li> + escaped name + the id markup
// fragments + two 20-digit ids + tail.
constexpr size_t kDeviceRowMax = sizeof("<li>") - 1 + devices::kNameCapacity * 5 + sizeof(kRowIdHead) - 1 + 20 +
                                 sizeof(kRowRemoveHead) - 1 + 20 + sizeof(kRowRemoveTail) - 1;
constexpr size_t kDeviceListMax = sizeof("<ul>") - 1 + devices::kMaxDevices * kDeviceRowMax + sizeof("</ul>") - 1;

// Page scratch for GET /: escaped SSID and the device list sit between the
// flash constants, so the page cannot be a single constant. Sized exactly
// (constants + worst-case escaping + digits + NUL), so snprintf can never
// truncate mid-tag. Single-task: handlers run on the main loop, one request
// at a time. Static so it never hits the small loop-task stack.
// Weather form additions: the template plus its four substituted values —
// " checked", the escaped name (5x), and two coordinate strings.
constexpr size_t kWeatherFormMax =
    sizeof(kWeatherForm) - 1 + (sizeof(" checked") - 1) + settings::kWeatherNameMaxLen * 5 + 16 + 16;

// WireGuard form additions: " checked", the escaped endpoint (5x), a port
// string (5), the tunnel IP verbatim (15, validated at entry) and a
// keepalive string (3).
constexpr size_t kWireGuardFormMax = sizeof(kWireGuardForm) - 1 + (sizeof(" checked") - 1) +
                                     settings::kWgEndpointMaxLen * 5 + 5 + settings::kWgIpMaxLen + 3;

// Network section worst case: head + the empty-list note + four fully
// escaped rows (each possibly followed by a portal-state note) + the add
// form with its slot digit. The add form and the full-list note are mutually
// exclusive; both are counted, over-sizing the static by a few tens of bytes
// for a formula that can never truncate.
constexpr size_t kNetRowMax =
    sizeof(kNetRowHead) - 1 + settings::kSsidMaxLen * 5 + sizeof(kNetRowMid) - 1 + 3 +
    (sizeof(kNetRowTailActive) > sizeof(kNetRowTailNormal) ? sizeof(kNetRowTailActive) : sizeof(kNetRowTailNormal)) -
    1 + 1;
constexpr size_t kNetStateMax = sizeof(kNetStateOpen) > sizeof(kNetStatePortal) ? sizeof(kNetStateOpen)
                                                                                : sizeof(kNetStatePortal);
constexpr size_t kNetSectionMax = sizeof(kNetHead) - 1 + sizeof(kNoneStored) - 1 +
                                  settings::kMaxNetworks * (kNetRowMax + kNetStateMax - 1) + sizeof(kNetAddForm) - 1 +
                                  3 + sizeof(kNetFullNote) - 1;

constexpr size_t kPageBufSize = kNetSectionMax + sizeof(kDevicesHead) - 1 + kDeviceListMax + sizeof(kDeviceForm) - 1 +
                                kWeatherFormMax + kWireGuardFormMax + sizeof(kFooter) - 1 + 1;
static char sPageBuf[kPageBufSize];

// Shared HTML-escape scratch: sized for the worst case of any input — the
// WireGuard endpoint (5x 64 chars) dominates the SSID (5x 32) and device
// names (5x 23).
static char sEscapedText[settings::kWgEndpointMaxLen * 5 + 1];

// Scratch for the one-entry device.json rebuilt from the /device form: worst
// case a 20-digit id, name escaped 2x, dotted-quad ip, 256-char token, 64-char
// key, overhead.
static char sDeviceDoc[640];

void sendPage(WebServer& server, int code, const char* page) {
  server.send_P(code, kContentType, page, strlen(page));
}

// Status/result pages: shared head (optionally with the 3 s auto-refresh
// meta) plus one constant body. Fits sPageBuf with wide margin.
void sendStatusPage(WebServer& server, int code, const char* head, const char* body) {
  snprintf(sPageBuf, kPageBufSize, "%s%s", head, body);
  sendPage(server, code, sPageBuf);
}

// Validates dotted-quad form (four 0..255 groups, nothing trailing) and
// copies the text verbatim on success.
bool dottedQuadArg(const String& s, char* out, size_t cap) {
  if (s.length() == 0 || s.length() > settings::kWgIpMaxLen) return false;
  char buf[settings::kWgIpMaxLen + 1];
  snprintf(buf, sizeof(buf), "%s", s.c_str());
  unsigned a = 0, b = 0, c = 0, d = 0;
  int consumed = 0;
  if (std::sscanf(buf, "%3u.%3u.%3u.%3u%n", &a, &b, &c, &d, &consumed) != 4 || buf[consumed] != '\0' || a > 255 ||
      b > 255 || c > 255 || d > 255) {
    return false;
  }
  snprintf(out, cap, "%s", buf);
  return true;
}

// E4 coordinate -> short decimal string ("" for the unset 0 sentinel), for
// prefilling the weather form.
void formatE4(int32_t v, char* buf, size_t cap) {
  if (v == 0) {
    buf[0] = '\0';
    return;
  }
  const bool neg = v < 0;
  const uint32_t a = neg ? -static_cast<uint32_t>(v) : static_cast<uint32_t>(v);
  snprintf(buf, cap, "%s%u.%04u", neg ? "-" : "", static_cast<unsigned>(a / 10000u), static_cast<unsigned>(a % 10000u));
}

// Form coordinate -> E4. Full-string parse (trailing spaces allowed): junk,
// NaN/inf or out-of-range input is rejected. strtod runs in the C locale,
// so '.' is always the decimal separator.
bool parseCoordE4Arg(const String& s, int32_t maxAbsE4, int32_t& out) {
  if (s.length() == 0 || s.length() > 15) return false;
  const char* c = s.c_str();
  char* end = nullptr;
  const double v = std::strtod(c, &end);
  while (end != nullptr && *end == ' ') {
    ++end;
  }
  if (end == nullptr || *end != '\0') return false;
  const double scaled = v * 10000.0;
  if (!(scaled >= -static_cast<double>(maxAbsE4) && scaled <= static_cast<double>(maxAbsE4))) return false;
  out = static_cast<int32_t>(llround(scaled));
  return true;
}

// Escapes the settings-file SSID before embedding it in the form attribute;
// worst case 5 bytes per source character (& -> &amp;).
void htmlEscape(const char* in, char* out, size_t cap) {
  size_t n = 0;
  for (const char* c = in; *c != '\0' && n + 6 < cap; ++c) {
    switch (*c) {
      case '&':
        n += snprintf(out + n, cap - n, "&amp;");
        break;
      case '<':
        n += snprintf(out + n, cap - n, "&lt;");
        break;
      case '>':
        n += snprintf(out + n, cap - n, "&gt;");
        break;
      case '"':
        n += snprintf(out + n, cap - n, "&quot;");
        break;
      default:
        out[n++] = *c;
    }
  }
  out[n] = '\0';
}

// Minimal JSON string escaping for the rebuilt device.json (quote and
// backslash). The parser additionally enforces printable-ASCII names.
void jsonEscape(const char* in, char* out, size_t cap) {
  size_t n = 0;
  for (const char* c = in; *c != '\0' && n + 2 < cap; ++c) {
    if (*c == '"' || *c == '\\') {
      out[n++] = '\\';
    }
    out[n++] = *c;
  }
  out[n] = '\0';
}

} // namespace

bool WebPortal::begin(SettingsStore& settingsStore, DeviceStore& deviceStore, bool closeOnIdle) {
  if (server_) return true;
  store_ = &settingsStore;
  deviceStore_ = &deviceStore;

  server_.reset(new (std::nothrow) WebServer(kPort));
  if (!server_) {
    Serial.println("[WEB] OOM creating web server, portal disabled");
    return false;
  }

  const uint32_t heapBefore = ESP.getFreeHeap();
  server_->on("/", HTTP_GET, [this] {
    noteRequest();
    handleRoot();
  });
  server_->on("/net", HTTP_POST, [this] {
    noteRequest();
    handleNetwork();
  });
  server_->on("/device", HTTP_POST, [this] {
    noteRequest();
    handleDeviceSave();
  });
  server_->on("/remove", HTTP_POST, [this] {
    noteRequest();
    handleDeviceRemove();
  });
  server_->on("/weather", HTTP_POST, [this] {
    noteRequest();
    handleWeather();
  });
  server_->on("/wireguard", HTTP_POST, [this] {
    noteRequest();
    handleWireGuard();
  });
  server_->onNotFound([this] {
    noteRequest();
    sendStatusPage(*server_, 404, kPageHead, kPageNotFound);
  });
  server_->begin(kPort);
  closeOnIdle_ = closeOnIdle;
  lastActivityMs_ = millis();
  Serial.printf("[WEB] portal on port %u (idle timeout %s), heap %u -> %u\n", kPort, closeOnIdle ? "15 min" : "off",
                heapBefore, ESP.getFreeHeap());
  return true;
}

void WebPortal::end() {
  if (!server_) return;
  // The WebServer destructor closes the listen socket (core WebServer.cpp
  // ~WebServer), so the reset is the full teardown.
  server_.reset();
  Serial.printf("[WEB] portal closed, heap %u\n", ESP.getFreeHeap());
}

void WebPortal::tick() {
  if (!server_) return;
  server_->handleClient();
  // Wrap-safe unsigned math; a request served by the handleClient() above
  // stamps lastActivityMs_ before this check, so it always resets the clock.
  if (closeOnIdle_ && millis() - lastActivityMs_ >= kIdleTimeoutMs) {
    Serial.println("[WEB] portal idle 15 min, closing");
    end();
  }
}

void WebPortal::handleRoot() {
  const settings::Settings& s = store_->settings();

  // kPageBufSize is the exact worst case of this composition; the clamped
  // offset keeps a future markup mistake truncating instead of corrupting.
  size_t off = 0;
  auto add = [&](const char* s) {
    const int n = snprintf(sPageBuf + off, kPageBufSize - off, "%s", s);
    if (n > 0 && off + static_cast<size_t>(n) < kPageBufSize) {
      off += static_cast<size_t>(n);
    }
  };

  add(kNetHead);
  if (!settings::anyNetwork(s)) add(kNoneStored);
  int8_t firstFree = -1;
  for (uint8_t i = 0; i < settings::kMaxNetworks; ++i) {
    const settings::Network& net = s.networks[i];
    if (net.ssid[0] == '\0') {
      if (firstFree < 0) firstFree = static_cast<int8_t>(i);
      continue;
    }
    htmlEscape(net.ssid, sEscapedText, sizeof(sEscapedText));
    const char* tail = s.activeNetwork == i ? kNetRowTailActive : kNetRowTailNormal;
    const int n = snprintf(sPageBuf + off, kPageBufSize - off, "%s%s%s%u%s", kNetRowHead, sEscapedText, kNetRowMid,
                           static_cast<unsigned>(i), tail);
    if (n > 0 && off + static_cast<size_t>(n) < kPageBufSize) {
      off += static_cast<size_t>(n);
    }
    if (net.portalState == settings::kPortalOpen) {
      add(kNetStateOpen);
    } else if (net.portalState == settings::kPortalFound) {
      add(kNetStatePortal);
    }
  }
  if (firstFree >= 0) {
    const int n = snprintf(sPageBuf + off, kPageBufSize - off, kNetAddForm, static_cast<unsigned>(firstFree));
    if (n > 0 && off + static_cast<size_t>(n) < kPageBufSize) {
      off += static_cast<size_t>(n);
    }
  } else {
    add(kNetFullNote);
  }

  add(kDevicesHead);
  const devices::List& list = deviceStore_->list();
  if (list.count == 0) {
    add(kNoneStored);
  } else {
    add("<ul>");
    for (uint8_t i = 0; i < list.count; ++i) {
      // Names only: token/key material is never rendered. The row carries an
      // inline POST /remove form (confirmed in the browser) for deletion.
      htmlEscape(list.devices[i].name, sEscapedText, sizeof(sEscapedText));
      const int n = snprintf(sPageBuf + off, kPageBufSize - off, "<li>%s%s%" PRIu64 "%s%" PRIu64 "%s", sEscapedText,
                             kRowIdHead, list.devices[i].id, kRowRemoveHead, list.devices[i].id, kRowRemoveTail);
      if (n > 0 && off + static_cast<size_t>(n) < kPageBufSize) {
        off += static_cast<size_t>(n);
      }
    }
    add("</ul>");
  }
  add(kDeviceForm);

  {
    // Weather form with the stored values substituted; coordinates render as
    // decimals (empty when unset), so a save without touching them keeps the
    // stored pair (blank means keep).
    const settings::Weather& w = store_->settings().weather;
    htmlEscape(w.name, sEscapedText, sizeof(sEscapedText));
    char latAttr[16];
    char lonAttr[16];
    formatE4(w.latE4, latAttr, sizeof(latAttr));
    formatE4(w.lonE4, lonAttr, sizeof(lonAttr));
    const int n = snprintf(sPageBuf + off, kPageBufSize - off, kWeatherForm, w.enabled ? " checked" : "", sEscapedText,
                           latAttr, lonAttr);
    if (n > 0 && off + static_cast<size_t>(n) < kPageBufSize) {
      off += static_cast<size_t>(n);
    }
  }

  {
    // WireGuard form (7.3): keys are never rendered back (blank keeps the
    // stored one); the stored tunnel IP is validated dotted-quad text, so it
    // needs no escaping.
    const settings::WireGuard& g = store_->settings().wireguard;
    htmlEscape(g.endpoint, sEscapedText, sizeof(sEscapedText));
    char portAttr[8] = "";
    char kaAttr[6];
    if (g.port != 0) {
      snprintf(portAttr, sizeof(portAttr), "%u", static_cast<unsigned>(g.port));
    }
    snprintf(kaAttr, sizeof(kaAttr), "%u", static_cast<unsigned>(g.keepalive));
    const int n = snprintf(sPageBuf + off, kPageBufSize - off, kWireGuardForm, g.enabled ? " checked" : "",
                           sEscapedText, portAttr, g.ownIp, kaAttr);
    if (n > 0 && off + static_cast<size_t>(n) < kPageBufSize) {
      off += static_cast<size_t>(n);
    }
  }

  add(kFooter);

  sendPage(*server_, 200, sPageBuf);
}

void WebPortal::handleNetwork() {
  // Every network form carries the hidden slot index and the clicked button's
  // action. A junk or out-of-range slot cannot happen from our own page: 404.
  const String slotArg = server_->arg("slot");
  const String action = server_->arg("action");
  char* end = nullptr;
  const unsigned long slotVal = slotArg.length() == 0 ? settings::kMaxNetworks : strtoul(slotArg.c_str(), &end, 10);
  if (slotVal >= settings::kMaxNetworks || (end != nullptr && *end != '\0')) {
    Serial.println("[WEB] rejected network form (slot)");
    sendStatusPage(*server_, 400, kPageHead, kPageNotFound);
    return;
  }
  const uint8_t slot = static_cast<uint8_t>(slotVal);
  settings::Settings& settings = store_->settings();
  settings::Network& net = settings.networks[slot];
  const bool isActive = settings.activeNetwork == slot;

  if (action == "connect") {
    if (net.ssid[0] == '\0') {
      sendStatusPage(*server_, 404, kPageHead, kPageNotFound);
      return;
    }
    if (isActive) {
      sendStatusPage(*server_, 200, kPageHead, kPageNetworkActive);
      return;
    }
    settings.activeNetwork = slot;
    if (!store_->saveNow()) {
      sendStatusPage(*server_, 200, kPageHead, kPageNotPersisted);
      return;
    }
    // Radio-affecting change: reboot to connect the new active profile.
    Serial.printf("[WEB] active network -> %s\n", net.ssid);
    sendStatusPage(*server_, 200, kPageHead, kPageNetworkReboot);
    delay(300);
    ESP.restart();
    return;
  }

  if (action == "delete") {
    if (net.ssid[0] == '\0') {
      sendStatusPage(*server_, 404, kPageHead, kPageNotFound);
      return;
    }
    net = settings::Network{};
    // The active index must keep naming a filled slot: fall back to the
    // first remaining profile, or none — with no networks left the reboot
    // lands in the provisioning flow.
    if (isActive) {
      settings.activeNetwork = settings::kNoNetwork;
      for (uint8_t i = 0; i < settings::kMaxNetworks; ++i) {
        if (settings.networks[i].ssid[0] != '\0') {
          settings.activeNetwork = i;
          break;
        }
      }
    }
    Serial.printf("[WEB] network removed (slot=%u, was_active=%d)\n", static_cast<unsigned>(slot),
                  static_cast<int>(isActive));
    if (!store_->saveNow()) {
      sendStatusPage(*server_, 200, kPageHead, kPageNotPersisted);
      return;
    }
    if (isActive) {
      // The link must move: only a reboot applies the fallback (or the
      // provisioning boot when the last network went away).
      sendStatusPage(*server_, 200, kPageHead, kPageNetworkReboot);
      delay(300);
      ESP.restart();
      return;
    }
    sendStatusPage(*server_, 200, kPageHead, kPageNetworkRemoved);
    return;
  }

  // Default verb: save (also the add form's button).
  const String ssidArg = server_->arg("ssid");
  const String passwordArg = server_->arg("password");
  // A saved network always needs a name; blank password keeps the stored one
  // (stays empty on a new profile = open network). Anything present must be
  // WPA2-legal.
  if (ssidArg.length() == 0 || ssidArg.length() > settings::kSsidMaxLen ||
      passwordArg.length() > settings::kPasswordMaxLen || (passwordArg.length() > 0 && passwordArg.length() < 8)) {
    Serial.println("[WEB] rejected network form (field length)");
    sendStatusPage(*server_, 400, kPageHead, kPageNetworkInvalid);
    return;
  }

  const bool ssidChanged = net.ssid[0] == '\0' || strcmp(net.ssid, ssidArg.c_str()) != 0;
  const bool passwordGiven = passwordArg.length() > 0;
  const bool passwordChanged = passwordGiven && strcmp(net.password, passwordArg.c_str()) != 0;
  if (!ssidChanged && !passwordChanged) {
    Serial.println("[WEB] network form, no changes");
    sendStatusPage(*server_, 200, kPageHead, kPageUnchanged);
    return;
  }

  snprintf(net.ssid, sizeof(net.ssid), "%s", ssidArg.c_str());
  if (passwordGiven) {
    snprintf(net.password, sizeof(net.password), "%s", passwordArg.c_str());
  }
  if (ssidChanged) {
    // Different network behind the slot: drop the captive-portal cache.
    net.portalState = settings::kPortalUnknown;
  }
  // Claim the active profile when nothing is set yet (fresh provisioning
  // store): the first saved network is the one to connect.
  bool claimed = false;
  if (settings.activeNetwork == settings::kNoNetwork) {
    settings.activeNetwork = slot;
    claimed = true;
  }
  // Never log the password itself.
  Serial.printf("[WEB] network saved (slot=%u, ssid=%s, password=%s)\n", static_cast<unsigned>(slot), net.ssid,
                passwordChanged ? "(updated)" : "(kept)");

  if (!store_->saveNow()) {
    // Stay up: the in-RAM settings still work this boot and the debounced
    // retry path is armed; only a committed save follows a reboot.
    sendStatusPage(*server_, 200, kPageHead, kPageNotPersisted);
    return;
  }

  // Only radio-affecting changes reboot (active profile's credentials, or a
  // just-claimed active on the provisioning boot). Other edits touch a
  // network the radio is not using, so the store is already consistent and
  // the user switches when they want to.
  if (claimed || (isActive && (ssidChanged || passwordChanged))) {
    sendStatusPage(*server_, 200, kPageHead, kPageNetworkReboot);
    delay(300);
    ESP.restart();
    return;
  }
  sendStatusPage(*server_, 200, kPageHead, kPageNetworkSaved);
}

void WebPortal::handleDeviceSave() {
  const String idArg = server_->arg("id");
  const String nameArg = server_->arg("name");
  const String ipArg = server_->arg("ip");
  const String tokenArg = server_->arg("token");
  const String keyArg = server_->arg("key");

  // Length gates matching the form's maxlength attributes; parseImportJson
  // below still performs the strict validation (single path).
  if (idArg.length() == 0 || idArg.length() > 20 || nameArg.length() >= devices::kNameCapacity || ipArg.length() > 15 ||
      tokenArg.length() > devices::kTokenMaxLen * 2 || keyArg.length() > devices::kKeyLen * 2) {
    Serial.println("[WEB] rejected device form (field length)");
    sendStatusPage(*server_, 400, kPageHead, kPageDeviceInvalid);
    return;
  }

  // Base 0 accepts decimal or 0x ids; 0 is not a valid msmart device id.
  const uint64_t id = strtoull(idArg.c_str(), nullptr, 0);
  if (id == 0) {
    Serial.println("[WEB] rejected device form (id)");
    sendStatusPage(*server_, 400, kPageHead, kPageDeviceInvalid);
    return;
  }

  // Editing an existing id: blank ip/token/key keep the stored values (upsert
  // below replaces the whole record, so carried fields are copied back into
  // the parsed device after validation). Token entered without its key still
  // hits the parser's kBadKey rejection — no partial credential updates.
  const devices::List& list = deviceStore_->list();
  const devices::Device* existing = nullptr;
  for (uint8_t i = 0; i < list.count; ++i) {
    if (list.devices[i].id == id) {
      existing = &list.devices[i];
      break;
    }
  }
  const bool keepCreds = tokenArg.length() == 0 && existing != nullptr && existing->hasCredentials();
  const bool keepIp = ipArg.length() == 0 && existing != nullptr &&
                      (existing->ip[0] | existing->ip[1] | existing->ip[2] | existing->ip[3]) != 0;

  // Rebuild a one-entry device.json and hand it to the SD import's parser, so
  // both entry paths validate identically. Token/key only ride along when a
  // token was given (blank = unauthenticated V1/V2 device); a token without
  // its key is rejected by the parser as kBadKey. sDeviceDoc is 640 B and the
  // length gates above bound every field, so these steps cannot truncate.
  jsonEscape(nameArg.c_str(), sEscapedText, sizeof(sEscapedText));
  int n = snprintf(sDeviceDoc, sizeof(sDeviceDoc), "{\"v\":1,\"devices\":[{\"id\":%" PRIu64 ",\"name\":\"%s\"", id,
                   sEscapedText);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(sDeviceDoc)) {
    Serial.println("[WEB] device doc build overflow");
    sendStatusPage(*server_, 400, kPageHead, kPageDeviceInvalid);
    return;
  }
  if (ipArg.length() > 0) {
    n += snprintf(sDeviceDoc + n, sizeof(sDeviceDoc) - n, ",\"ip\":\"%s\"", ipArg.c_str());
  }
  if (tokenArg.length() > 0) {
    n += snprintf(sDeviceDoc + n, sizeof(sDeviceDoc) - n, ",\"token\":\"%s\"", tokenArg.c_str());
    if (keyArg.length() > 0) {
      n += snprintf(sDeviceDoc + n, sizeof(sDeviceDoc) - n, ",\"key\":\"%s\"", keyArg.c_str());
    }
  }
  n += snprintf(sDeviceDoc + n, sizeof(sDeviceDoc) - n, "}]}");

  static devices::List parsed; // main-loop only; untouched by the parser on failure
  const devices::ImportError err = devices::parseImportJson(sDeviceDoc, parsed);
  if (err != devices::ImportError::kNone) {
    Serial.printf("[WEB] device rejected (reason %u)\n", static_cast<unsigned>(err));
    sendStatusPage(*server_, 400, kPageHead, kPageDeviceInvalid);
    return;
  }
  if (keepCreds) {
    std::memcpy(parsed.devices[0].token, existing->token, devices::kTokenMaxLen);
    parsed.devices[0].tokenLen = existing->tokenLen;
    std::memcpy(parsed.devices[0].key, existing->key, devices::kKeyLen);
  }
  if (keepIp) {
    std::memcpy(parsed.devices[0].ip, existing->ip, 4);
  }

  const int index = deviceStore_->upsert(parsed.devices[0]);
  if (index < 0) {
    Serial.println("[WEB] device store full");
    sendStatusPage(*server_, 400, kPageHead, kPageDeviceFull);
    return;
  }
  if (!deviceStore_->saveNow()) {
    // Stay up on a failed write (retry path armed); reboot only follows a
    // committed save, mirroring the portal's persist-then-reboot contract.
    sendStatusPage(*server_, 200, kPageHead, kPageNotPersisted);
    return;
  }

  // Names and lengths only: token/key material is never logged.
  const devices::Device& saved = parsed.devices[0];
  Serial.printf("[WEB] device saved (id=%" PRIu64 ", name=%s, token=%u B, ip=%u.%u.%u.%u)\n", saved.id, saved.name,
                saved.tokenLen, static_cast<unsigned>(saved.ip[0]), static_cast<unsigned>(saved.ip[1]),
                static_cast<unsigned>(saved.ip[2]), static_cast<unsigned>(saved.ip[3]));
  sendStatusPage(*server_, 200, kPageHead, kPageDeviceSaved);
  delay(300);
  ESP.restart();
}

void WebPortal::handleWeather() {
  const String enArg = server_->arg("en");
  const String nameArg = server_->arg("name");
  const String latArg = server_->arg("lat");
  const String lonArg = server_->arg("lon");

  settings::Settings& settings = store_->settings();
  // Build the candidate first: any invalid field rejects the whole form and
  // the live settings stay untouched (same all-or-nothing contract as WiFi).
  settings::Weather next = settings.weather;
  if (nameArg.length() > settings::kWeatherNameMaxLen) {
    Serial.println("[WEB] rejected weather form (name length)");
    sendStatusPage(*server_, 400, kPageHead, kPageWeatherInvalid);
    return;
  }
  // Blank coordinate fields keep the stored values (same contract as the
  // WiFi password field); anything present must parse and be in range.
  if (latArg.length() > 0 && !parseCoordE4Arg(latArg, settings::kLatMaxE4, next.latE4)) {
    Serial.println("[WEB] rejected weather form (latitude)");
    sendStatusPage(*server_, 400, kPageHead, kPageWeatherInvalid);
    return;
  }
  if (lonArg.length() > 0 && !parseCoordE4Arg(lonArg, settings::kLonMaxE4, next.lonE4)) {
    Serial.println("[WEB] rejected weather form (longitude)");
    sendStatusPage(*server_, 400, kPageHead, kPageWeatherInvalid);
    return;
  }
  // The checkbox is only submitted when checked.
  next.enabled = enArg.length() > 0;
  if (next.enabled && !settings::weatherConfigured(next)) {
    Serial.println("[WEB] rejected weather form (enabled without location)");
    sendStatusPage(*server_, 400, kPageHead, kPageWeatherInvalid);
    return;
  }
  snprintf(next.name, sizeof(next.name), "%s", nameArg.c_str());

  const bool changed = next.enabled != settings.weather.enabled || std::strcmp(next.name, settings.weather.name) != 0 ||
                       next.latE4 != settings.weather.latE4 || next.lonE4 != settings.weather.lonE4;
  if (!changed) {
    Serial.println("[WEB] weather form submitted, no changes");
  }

  settings.weather = next;
  Serial.printf("[WEB] weather settings %s (enabled=%d, latE4=%" PRId32 ", lonE4=%" PRId32 ")\n",
                changed ? "updated" : "unchanged", static_cast<int>(next.enabled), next.latE4, next.lonE4);

  const bool persisted = store_->saveNow();
  if (!persisted) {
    sendStatusPage(*server_, 200, kPageHead, kPageNotPersisted);
    return;
  }
  // No reboot: unlike credentials, weather settings apply hot — the weather
  // task re-reads them, and the panel appears with the next snapshot.
  sendStatusPage(*server_, 200, kPageHead, kPageWeatherSaved);
}

void WebPortal::handleWireGuard() {
  const String enArg = server_->arg("en");
  const String epArg = server_->arg("ep");
  const String portArg = server_->arg("port");
  const String privArg = server_->arg("priv");
  const String pubArg = server_->arg("pub");
  const String ipArg = server_->arg("tip");
  const String kaArg = server_->arg("ka");

  settings::Settings& settings = store_->settings();
  // Build the candidate first: any invalid field rejects the whole form and
  // the live settings stay untouched (same contract as weather). The service
  // diffs the block on its tick, so an accepted save applies hot.
  settings::WireGuard next = settings.wireguard;
  auto reject = [this](const char* why) {
    Serial.printf("[WEB] rejected wireguard form (%s)\n", why);
    sendStatusPage(*server_, 400, kPageHead, kPageWireGuardInvalid);
  };

  if (epArg.length() > settings::kWgEndpointMaxLen) {
    reject("endpoint length");
    return;
  }
  if (epArg.length() > 0) {
    snprintf(next.endpoint, sizeof(next.endpoint), "%s", epArg.c_str());
  }
  if (portArg.length() > 0) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(portArg.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || v < 1 || v > 65535) {
      reject("port");
      return;
    }
    next.port = static_cast<uint16_t>(v);
  }
  // Keys: blank keeps the stored one (never rendered back); anything else
  // must carry exactly 44 characters. Whether the base64 decodes is left to
  // the service — a bad key shows up as a failed tunnel, not a form reject.
  if (privArg.length() > 0) {
    if (privArg.length() != settings::kWgKeyLen) {
      reject("private key length");
      return;
    }
    snprintf(next.ownPrivateKey, sizeof(next.ownPrivateKey), "%s", privArg.c_str());
  }
  if (pubArg.length() > 0) {
    if (pubArg.length() != settings::kWgKeyLen) {
      reject("public key length");
      return;
    }
    snprintf(next.peerPublicKey, sizeof(next.peerPublicKey), "%s", pubArg.c_str());
  }
  if (ipArg.length() > 0 && !dottedQuadArg(ipArg, next.ownIp, sizeof(next.ownIp))) {
    reject("tunnel ip");
    return;
  }
  if (kaArg.length() > 0) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(kaArg.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || v > settings::kWgKeepaliveMaxSecs) {
      reject("keepalive");
      return;
    }
    next.keepalive = static_cast<uint16_t>(v);
  }
  // The checkbox is only submitted when checked.
  next.enabled = enArg.length() > 0;
  if (next.enabled && !settings::wireGuardConfigured(next)) {
    reject("incomplete");
    return;
  }

  const bool changed = std::memcmp(&next, &settings.wireguard, sizeof(next)) != 0;
  settings.wireguard = next;
  Serial.printf("[WEB] wireguard settings %s (enabled=%d)\n", changed ? "updated" : "unchanged",
                static_cast<int>(next.enabled));

  const bool persisted = store_->saveNow();
  if (!persisted) {
    sendStatusPage(*server_, 200, kPageHead, kPageNotPersisted);
    return;
  }
  sendStatusPage(*server_, 200, kPageHead, kPageWireGuardSaved);
}

void WebPortal::handleDeviceRemove() {
  // Decimal id from the row's hidden field; strtoull yielding 0 (empty or
  // junk) is invalid — a stored id is never 0.
  const String idArg = server_->arg("id");
  const uint64_t id = idArg.length() == 0 ? 0 : strtoull(idArg.c_str(), nullptr, 10);
  if (id == 0) {
    sendStatusPage(*server_, 400, kPageHead, kPageDeviceInvalid);
    return;
  }
  if (!deviceStore_->remove(id)) {
    Serial.printf("[WEB] remove: id %" PRIu64 " not stored\n", id);
    sendStatusPage(*server_, 404, kPageHead, kPageNotFound);
    return;
  }
  if (!deviceStore_->saveNow()) {
    // Same persist-then-reboot contract as the saves: stay up on a failed
    // write; the removal is then lost on reboot.
    sendStatusPage(*server_, 200, kPageHead, kPageNotPersisted);
    return;
  }

  Serial.printf("[WEB] device removed (id=%" PRIu64 ")\n", id);
  sendStatusPage(*server_, 200, kPageHead, kPageDeviceRemoved);
  delay(300);
  ESP.restart();
}
