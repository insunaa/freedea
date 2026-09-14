#include <HtmlForm.h>

#include <cstdio>
#include <cstring>

namespace htmlform {
namespace {

constexpr size_t kNone = SIZE_MAX;

char lowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool ciEq(const char* s, size_t n, const char* lit) {
  size_t i = 0;
  for (; lit[i] != '\0' && i < n; ++i) {
    if (lowerAscii(s[i]) != lit[i]) return false;
  }
  return lit[i] == '\0' && i == n;
}

bool isWs(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

struct Span {
  size_t off;
  size_t len;
};

// First tag start "<name" (case-insensitive) at or after `from`; the name
// must be followed by whitespace, "/", or ">" (so <formx and </form do not
// match). kNone when absent.
size_t findTag(const char* h, size_t len, size_t from, const char* name) {
  const size_t n = std::strlen(name);
  for (size_t i = from; i < len; ++i) {
    if (h[i] != '<') continue;
    if (i + 1 + n > len) break;
    if (!ciEq(h + i + 1, n, name)) continue;
    if (i + 1 + n < len) {
      const char nx = h[i + 1 + n];
      if (!isWs(nx) && nx != '>' && nx != '/') continue;
    }
    return i;
  }
  return kNone;
}

// First "</name" at or after `from` (whitespace between '<' and '/' is
// tolerated). kNone when absent.
size_t findCloseTag(const char* h, size_t len, size_t from, const char* name) {
  const size_t n = std::strlen(name);
  for (size_t i = from; i < len; ++i) {
    if (h[i] != '<') continue;
    size_t j = i + 1;
    while (j < len && isWs(h[j]))
      ++j;
    if (j >= len || h[j] != '/') continue;
    ++j;
    if (j + n > len || !ciEq(h + j, n, name)) continue;
    if (j + n < len) {
      const char nx = h[j + n];
      if (!isWs(nx) && nx != '>') continue;
    }
    return i;
  }
  return kNone;
}

// The tag's closing '>' (quote-aware so a '>' inside an attribute value does
// not end the tag). kNone when the tag never closes.
size_t findTagEnd(const char* h, size_t len, size_t lt) {
  char quote = 0;
  for (size_t i = lt + 1; i < len; ++i) {
    if (quote != 0) {
      if (h[i] == quote) quote = 0;
    } else if (h[i] == '"' || h[i] == '\'') {
      quote = h[i];
    } else if (h[i] == '>') {
      return i;
    }
  }
  return kNone;
}

// First attribute named `want` inside the tag text [lt, end) (first
// occurrence wins, values may be quoted or bare). Returns true when the
// attribute exists with a value; hadAttr reports mere presence (for
// boolean attributes like checked/disabled, whose value span is meaningless).
bool findAttr(const char* h, size_t lt, size_t end, const char* want, Span& value, bool& hadAttr) {
  bool found = false;
  bool valuePresent = false;
  size_t i = lt + 1;
  while (i < end && !isWs(h[i]) && h[i] != '>' && h[i] != '/')
    ++i; // skip tag name
  while (i < end) {
    while (i < end && (isWs(h[i]) || h[i] == '/'))
      ++i;
    if (i >= end) break;
    const size_t ns = i;
    while (i < end && h[i] != '=' && !isWs(h[i]) && h[i] != '>' && h[i] != '/')
      ++i;
    if (i == ns) { // stray character
      ++i;
      continue;
    }
    const bool isMatch = !found && ciEq(h + ns, i - ns, want);
    bool curHasValue = false;
    Span vs{};
    if (i < end && h[i] == '=') {
      ++i;
      while (i < end && isWs(h[i]))
        ++i;
      if (i < end && (h[i] == '"' || h[i] == '\'')) {
        const char q = h[i++];
        vs.off = i;
        while (i < end && h[i] != q)
          ++i;
        vs.len = i - vs.off;
        if (i < end) ++i;
      } else {
        vs.off = i;
        while (i < end && !isWs(h[i]) && h[i] != '>')
          ++i;
        vs.len = i - vs.off;
      }
      curHasValue = true;
    }
    if (isMatch) {
      value = vs;
      valuePresent = curHasValue;
      found = true;
    }
    if (curHasValue) continue; // loop continues scanning from the value end
  }
  hadAttr = found;
  return found && valuePresent;
}

void appendUtf8(uint32_t cp, char* out, size_t cap, size_t& n) {
  if (cp == 0 || cp > 0x10FFFF) return;
  if (cp < 0x80) {
    if (n + 1 < cap) out[n++] = static_cast<char>(cp);
  } else if (cp < 0x800) {
    if (n + 2 < cap) {
      out[n++] = static_cast<char>(0xC0 | (cp >> 6));
      out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
  } else if (cp < 0x10000) {
    if (n + 3 < cap) {
      out[n++] = static_cast<char>(0xE0 | (cp >> 12));
      out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
  } else if (n + 4 < cap) {
    out[n++] = static_cast<char>(0xF0 | (cp >> 18));
    out[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// HTML-entity decode into a NUL-terminated buffer (truncated on overflow):
// the five common named entities plus decimal/hex numeric references.
void decodeEntities(const char* src, size_t len, char* out, size_t cap) {
  if (cap == 0) return;
  size_t n = 0;
  for (size_t i = 0; i < len;) {
    if (src[i] != '&' || i + 1 >= len) {
      if (n + 1 < cap) out[n++] = src[i];
      ++i;
      continue;
    }
    size_t j = i + 1;
    uint32_t cp = 0;
    bool decoded = false;
    if (src[j] == '#') {
      ++j;
      const bool hex = j < len && (src[j] == 'x' || src[j] == 'X');
      if (hex) ++j;
      const size_t digitsStart = j;
      while (j < len) {
        const char c = src[j];
        uint32_t d;
        if (c >= '0' && c <= '9')
          d = static_cast<uint32_t>(c - '0');
        else if (hex && c >= 'a' && c <= 'f')
          d = static_cast<uint32_t>(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F')
          d = static_cast<uint32_t>(c - 'A' + 10);
        else
          break;
        cp = cp * (hex ? 16u : 10u) + d;
        ++j;
      }
      if (j > digitsStart) {
        if (j < len && src[j] == ';') ++j; // semicolon tolerated as optional
        decoded = true;
      }
    } else {
      struct Named {
        const char* text;
        char ch;
      };
      static constexpr Named kNamed[] = {{"amp;", '&'},  {"lt;", '<'},    {"gt;", '>'},
                                         {"quot;", '"'}, {"apos;", '\''}, {"nbsp;", ' '}};
      for (const Named& e : kNamed) {
        const size_t el = std::strlen(e.text);
        if (j + el <= len && std::strncmp(src + j, e.text, el) == 0) {
          cp = static_cast<uint8_t>(e.ch);
          j += el;
          decoded = true;
          break;
        }
      }
    }
    if (decoded) {
      appendUtf8(cp, out, cap, n);
      i = j;
    } else {
      if (n + 1 < cap) out[n++] = src[i]; // a stray '&' stays literal
      ++i;
    }
  }
  out[n] = '\0';
}

Span trim(const char* h, Span s) {
  while (s.len > 0 && isWs(h[s.off])) {
    ++s.off;
    --s.len;
  }
  while (s.len > 0 && isWs(h[s.off + s.len - 1]))
    --s.len;
  return s;
}

// Commits one field; false when the store is full or the decoded name is
// empty. Values truncate silently (a broken over-long token degrades to the
// caller's "could not join" path).
bool addField(Form& f, const char* h, Span name, const char* lit) {
  if (f.fieldCount >= kMaxFields) return false;
  Field& fld = f.fields[f.fieldCount];
  decodeEntities(h + name.off, name.len, fld.name, sizeof(fld.name));
  if (fld.name[0] == '\0') return false;
  std::snprintf(fld.value, sizeof(fld.value), "%s", lit);
  ++f.fieldCount;
  return true;
}

bool addField(Form& f, const char* h, Span name, Span value) {
  if (f.fieldCount >= kMaxFields) return false;
  Field& fld = f.fields[f.fieldCount];
  decodeEntities(h + name.off, name.len, fld.name, sizeof(fld.name));
  if (fld.name[0] == '\0') return false;
  decodeEntities(h + value.off, value.len, fld.value, sizeof(fld.value));
  ++f.fieldCount;
  return true;
}

bool tagIs(const char* h, size_t end, size_t t, const char* name) {
  const size_t n = std::strlen(name);
  if (t + 1 + n > end || !ciEq(h + t + 1, n, name)) return false;
  if (t + 1 + n < end) {
    const char nx = h[t + 1 + n];
    if (!isWs(nx) && nx != '>' && nx != '/') return false;
  }
  return true;
}

void handleInput(const char* h, size_t lt, size_t end, Form& out) {
  Span type{}, name{}, value{};
  bool hasType = false, hasName = false, hasValue = false;
  findAttr(h, lt, end, "type", type, hasType);
  findAttr(h, lt, end, "name", name, hasName);
  findAttr(h, lt, end, "value", value, hasValue);
  Span dummy{};
  bool checkedPresent = false, disabledPresent = false;
  findAttr(h, lt, end, "checked", dummy, checkedPresent);
  findAttr(h, lt, end, "disabled", dummy, disabledPresent);
  if (!hasName || disabledPresent) return;

  const bool noValueAttr = !hasValue || value.len == 0;
  if (hasType && ciEq(h + type.off, type.len, "checkbox")) {
    if (noValueAttr)
      addField(out, h, name, "on");
    else
      addField(out, h, name, value);
  } else if (hasType && ciEq(h + type.off, type.len, "radio")) {
    if (checkedPresent) {
      if (noValueAttr)
        addField(out, h, name, "on");
      else
        addField(out, h, name, value);
    }
  } else if (hasType && ciEq(h + type.off, type.len, "hidden")) {
    if (noValueAttr)
      addField(out, h, name, "");
    else
      addField(out, h, name, value);
  } else if (hasType && (ciEq(h + type.off, type.len, "submit") || ciEq(h + type.off, type.len, "image"))) {
    if (noValueAttr)
      addField(out, h, name, "on");
    else
      addField(out, h, name, value);
  } else if (hasType && (ciEq(h + type.off, type.len, "reset") || ciEq(h + type.off, type.len, "file") ||
                         ciEq(h + type.off, type.len, "button"))) {
    return;
  } else if (!noValueAttr) {
    // text-like input: only pre-filled values contribute
    addField(out, h, name, value);
  }
}

void handleButton(const char* h, size_t lt, size_t tagEnd, Span content, Form& out) {
  Span type{};
  bool hasType = false;
  findAttr(h, lt, tagEnd, "type", type, hasType);
  if (hasType && !ciEq(h + type.off, type.len, "submit")) return;
  Span name{}, value{};
  bool hasName = false, hasValue = false;
  findAttr(h, lt, tagEnd, "name", name, hasName);
  findAttr(h, lt, tagEnd, "value", value, hasValue);
  if (!hasName) return;
  if (hasValue) {
    addField(out, h, name, value);
  } else {
    const Span text = trim(h, content);
    addField(out, h, name, text);
  }
}

void handleTextarea(const char* h, size_t lt, size_t tagEnd, Span content, Form& out) {
  Span name{};
  bool hasName = false;
  findAttr(h, lt, tagEnd, "name", name, hasName);
  if (!hasName) return;
  const Span text = trim(h, content);
  if (text.len == 0) return; // empty textareas contribute nothing
  addField(out, h, name, text);
}

// ---- URL handling ----------------------------------------------------------

struct Appender {
  char* buf;
  size_t cap;
  size_t n;
  bool ok;

  explicit Appender(char* b, size_t c) : buf(b), cap(c), n(0), ok(c > 0) {
    if (ok) buf[0] = '\0';
  }
  void put(char c) {
    if (n + 1 >= cap) {
      ok = false;
      return;
    }
    buf[n++] = c;
    buf[n] = '\0';
  }
  void putRaw(const char* s, size_t len) {
    if (n + len >= cap) {
      ok = false;
      return;
    }
    std::memcpy(buf + n, s, len);
    n += len;
    buf[n] = '\0';
  }
};

bool isUnreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c == '.' || c == '~' || c == '*';
}

void encodeInto(Appender& a, const char* s) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  for (const char* p = s; *p != '\0'; ++p) {
    const char c = *p;
    if (isUnreserved(c)) {
      a.put(c);
    } else if (c == ' ') {
      a.put('+');
    } else {
      const uint8_t b = static_cast<uint8_t>(c);
      a.put('%');
      a.put(kHex[b >> 4]);
      a.put(kHex[b & 0x0F]);
    }
  }
}

bool startsWithCi(const char* s, size_t len, const char* lit) {
  const size_t n = std::strlen(lit);
  return len >= n && ciEq(s, n, lit);
}

} // namespace

bool parseFirstForm(const char* html, size_t len, Form& out) {
  if (html == nullptr || len == 0) return false;
  const size_t lt = findTag(html, len, 0, "form");
  if (lt == kNone) return false;
  const size_t gt = findTagEnd(html, len, lt);
  const size_t tagEnd = (gt == kNone) ? len : gt + 1;

  out = Form{};
  Span action{};
  bool hadAction = false;
  findAttr(html, lt, tagEnd, "action", action, hadAction);
  if (hadAction) decodeEntities(html + action.off, action.len, out.action, sizeof(out.action));
  Span method{};
  bool hadMethod = false;
  findAttr(html, lt, tagEnd, "method", method, hadMethod);
  if (hadMethod) out.post = ciEq(html + method.off, method.len, "post");

  const size_t region = tagEnd;
  size_t regionEnd = findCloseTag(html, len, region, "form");
  if (regionEnd == kNone) regionEnd = len; // unclosed form: parse what is there

  size_t i = region;
  while (i < regionEnd) {
    while (i < regionEnd && html[i] != '<')
      ++i;
    if (i >= regionEnd) break;
    if (tagIs(html, regionEnd, i, "input")) {
      const size_t tEnd = findTagEnd(html, regionEnd, i);
      const size_t end = (tEnd == kNone) ? regionEnd : tEnd + 1;
      handleInput(html, i, end, out);
      i = end;
    } else if (tagIs(html, regionEnd, i, "button")) {
      const size_t tEnd = findTagEnd(html, regionEnd, i);
      const size_t contentStart = (tEnd == kNone) ? regionEnd : tEnd + 1;
      const size_t close = findCloseTag(html, regionEnd, contentStart, "button");
      const Span content{contentStart, ((close == kNone) ? regionEnd : close) - contentStart};
      handleButton(html, i, (tEnd == kNone) ? regionEnd : tEnd, content, out);
      i = (close == kNone) ? regionEnd : close + 1;
    } else if (tagIs(html, regionEnd, i, "textarea")) {
      const size_t tEnd = findTagEnd(html, regionEnd, i);
      const size_t contentStart = (tEnd == kNone) ? regionEnd : tEnd + 1;
      const size_t close = findCloseTag(html, regionEnd, contentStart, "textarea");
      const Span content{contentStart, ((close == kNone) ? regionEnd : close) - contentStart};
      handleTextarea(html, i, (tEnd == kNone) ? regionEnd : tEnd, content, out);
      i = (close == kNone) ? regionEnd : close + 1;
    } else {
      ++i;
    }
  }
  return true;
}

bool resolveUrl(const char* target, const char* pageUrl, char* out, size_t outCap) {
  if (target == nullptr || pageUrl == nullptr || out == nullptr || outCap == 0) return false;
  const size_t pLen = std::strlen(pageUrl);
  if (!startsWithCi(pageUrl, pLen, "http://")) return false;

  // Page parts: authority, then path, then optional query; fragments drop.
  size_t authEnd = 7; // strlen("http://")
  while (authEnd < pLen && pageUrl[authEnd] != '/' && pageUrl[authEnd] != '?' && pageUrl[authEnd] != '#')
    ++authEnd;
  if (authEnd == 7) return false; // no host in the page URL
  const size_t pathStart = authEnd;
  size_t pathEnd = pathStart;
  while (pathEnd < pLen && pageUrl[pathEnd] != '?' && pageUrl[pathEnd] != '#')
    ++pathEnd;
  size_t queryStart = kNone;
  if (pathEnd < pLen && pageUrl[pathEnd] == '?') {
    queryStart = pathEnd + 1; // first query character (past '?')
    size_t q = queryStart;
    while (q < pLen && pageUrl[q] != '#')
      ++q;
    pathEnd = q;
  }
  const size_t pathLen = (queryStart != kNone ? queryStart - 1 : pathEnd) - pathStart;
  const bool pathPresent = pathLen > 0; // pageUrl[authEnd] == '/' when present

  size_t tLen = std::strlen(target);
  for (size_t i = 0; i < tLen; ++i) {
    if (target[i] == '#') {
      tLen = i;
      break;
    }
  }

  Appender a(out, outCap);
  if (tLen >= 2 && target[0] == '/' && target[1] == '/') { // scheme-relative
    a.putRaw("http:", 5);
    a.putRaw(target, tLen);
    return a.ok;
  }
  // Absolute or other scheme: scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":"
  if (tLen > 0 && ((target[0] >= 'A' && target[0] <= 'Z') || (target[0] >= 'a' && target[0] <= 'z'))) {
    size_t s = 0;
    while (s < tLen &&
           ((target[s] >= 'A' && target[s] <= 'Z') || (target[s] >= 'a' && target[s] <= 'z') ||
            (target[s] >= '0' && target[s] <= '9') || target[s] == '+' || target[s] == '-' || target[s] == '.')) {
      ++s;
    }
    if (s < tLen && target[s] == ':') {
      if (!ciEq(target, s, "http")) return false; // https: and friends: unsupported
      if (!(tLen >= s + 5 && startsWithCi(target + s, tLen - s, "://"))) return false;
      a.putRaw(target, tLen);
      return a.ok;
    }
  }
  // Relative targets resolve against the page URL.
  a.putRaw(pageUrl, authEnd); // "http://host[:port]"
  if (tLen == 0) {
    // Empty action: the document URL itself (query kept; GET strips it later).
    a.putRaw(pageUrl + pathStart, pathLen);
    if (queryStart != kNone) {
      a.put('?');
      a.putRaw(pageUrl + queryStart, pathEnd - queryStart);
    }
    return a.ok;
  }
  if (target[0] == '?') {
    a.putRaw(pageUrl + pathStart, pathLen);
    a.putRaw(target, tLen);
    return a.ok;
  }
  if (target[0] == '/') {
    a.putRaw(target, tLen);
    return a.ok;
  }
  // Path-relative: keep the page path up to its last '/'.
  if (!pathPresent) {
    a.put('/');
  } else {
    size_t keep = pathLen; // characters of the path to keep
    for (size_t i = pathLen; i > 0; --i) {
      if (pageUrl[pathStart + i - 1] == '/') break;
      --keep;
    }
    a.putRaw(pageUrl + pathStart, keep);
  }
  size_t tOff = 0;
  if (tLen >= 2 && target[0] == '.' && target[1] == '/') tOff = 2; // tolerate a leading ./
  a.putRaw(target + tOff, tLen - tOff);
  return a.ok;
}

bool buildRequest(const Form& form, const char* pageUrl, char* url, size_t urlCap, char* body, size_t bodyCap) {
  if (url == nullptr || body == nullptr || bodyCap == 0) return false;
  if (!resolveUrl(form.action, pageUrl, url, urlCap)) return false;
  body[0] = '\0';

  if (form.post) {
    Appender b(body, bodyCap);
    for (uint8_t i = 0; i < form.fieldCount; ++i) {
      if (i > 0) b.put('&');
      encodeInto(b, form.fields[i].name);
      b.put('=');
      encodeInto(b, form.fields[i].value);
    }
    return b.ok;
  }

  // GET replaces the whole query string: drop any action query first.
  char* q = std::strchr(url, '?');
  if (q != nullptr) *q = '\0';
  if (form.fieldCount == 0) return true; // GET with nothing to append
  const size_t urlLen = std::strlen(url);
  if (urlLen + 1 >= urlCap) return false;
  Appender u(url + urlLen, urlCap - urlLen);
  u.put('?');
  for (uint8_t i = 0; i < form.fieldCount; ++i) {
    if (i > 0) u.put('&');
    encodeInto(u, form.fields[i].name);
    u.put('=');
    encodeInto(u, form.fields[i].value);
  }
  return u.ok;
}

bool encodeValue(const char* value, char* out, size_t cap) {
  if (value == nullptr || out == nullptr || cap == 0) return false;
  Appender a(out, cap);
  encodeInto(a, value);
  return a.ok;
}

} // namespace htmlform
