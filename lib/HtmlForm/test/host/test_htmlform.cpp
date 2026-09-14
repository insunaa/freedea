// Host tests for lib/HtmlForm (7.2d): first-form extraction with the
// auto-login fill rules, URL resolution, and urlencoded request building.
// Fixtures are representative captive-portal pages (checkbox-accept forms,
// hidden tokens, uppercase markup, malformed tails).

#include <HtmlForm.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

void expectFields(const htmlform::Form& f, const char* expected[][2], size_t n, const char* what) {
  bool ok = f.fieldCount == n;
  for (size_t i = 0; ok && i < n; ++i) {
    ok = std::strcmp(f.fields[i].name, expected[i][0]) == 0 && std::strcmp(f.fields[i].value, expected[i][1]) == 0;
  }
  if (!ok) {
    std::printf("FAIL: %s (got %u fields:", what, static_cast<unsigned>(f.fieldCount));
    for (uint8_t i = 0; i < f.fieldCount; ++i)
      std::printf(" %s=%s", f.fields[i].name, f.fields[i].value);
    std::printf(")\n");
    ++failures;
  }
}

// A realistic "tick the box" train-portal page: hidden token with an
// escaped entity, unnamed checkbox, an empty and a pre-filled text input,
// and a submit button. A second form must be ignored.
void testTrainPortal() {
  const char* page = "<!DOCTYPE html><html><head><title>TrainConnect</title></head><body>"
                     "<h1>TrainConnect</h1>"
                     "<form method=\"post\" action=\"/portal/login?lang=en\">"
                     "<input type=\"hidden\" name=\"tok\" value=\"aBc&amp;12\">"
                     "<label><input type=\"checkbox\" name=\"terms\"> I accept the terms</label>"
                     "<input type=\"text\" name=\"room\" value=\"\">"
                     "<input type=\"text\" name=\"code\" value=\"1234\">"
                     "<input type=\"submit\" name=\"go\" value=\"Connect\">"
                     "</form>"
                     "<form method=\"get\" action=\"/ignored\"><input name=x value=1></form>"
                     "</body></html>";
  htmlform::Form form;
  check(htmlform::parseFirstForm(page, std::strlen(page), form), "train portal parses");
  check(form.post, "train portal is POST");
  check(std::strcmp(form.action, "/portal/login?lang=en") == 0, "train portal action kept");
  const char* want[][2] = {{"tok", "aBc&12"}, {"terms", "on"}, {"code", "1234"}, {"go", "Connect"}};
  expectFields(form, want, 4, "train portal fields");

  char url[htmlform::kMaxUrl], body[htmlform::kMaxBody];
  check(htmlform::buildRequest(form, "http://10.1.2.3/generate_204", url, sizeof(url), body, sizeof(body)),
        "train portal request builds");
  check(std::strcmp(url, "http://10.1.2.3/portal/login?lang=en") == 0, "train portal url");
  check(std::strcmp(body, "tok=aBc%2612&terms=on&code=1234&go=Connect") == 0, "train portal body");
}

// Checkbox-only GET form with an empty action: targets the document URL
// minus fragment/query (GET replaces the query).
void testCheckboxGet() {
  const char* page = "<form action=\"\"><input type=checkbox name=accept><input type=submit name=submit></form>";
  htmlform::Form form;
  check(htmlform::parseFirstForm(page, std::strlen(page), form), "checkbox get parses");
  check(!form.post, "checkbox get defaults to GET");
  const char* want[][2] = {{"accept", "on"}, {"submit", "on"}};
  expectFields(form, want, 2, "checkbox get fields");

  char url[htmlform::kMaxUrl], body[htmlform::kMaxBody];
  check(htmlform::buildRequest(form, "http://gate.local/start.php?next=1#frag", url, sizeof(url), body, sizeof(body)),
        "checkbox get builds");
  check(std::strcmp(url, "http://gate.local/start.php?accept=on&submit=on") == 0, "checkbox get url replaces query");
  check(std::strcmp(body, "") == 0, "checkbox get body empty");
}

// Uppercase/single-quoted markup, radio groups, disabled inputs, textarea
// content, and <button> text content.
void testUppercaseMixed() {
  const char* page = "<FORM METHOD=POST ACTION='/a/b'>"
                     "<INPUT TYPE=RADIO NAME=plan VALUE=basic>"
                     "<INPUT TYPE=radio name=plan value=premium CHECKED>"
                     "<input name=off disabled value=x>"
                     "<textarea name=msg>  hi &amp; bye  </textarea>"
                     "<button name=ok type=submit> Go online </button>"
                     "</form>";
  htmlform::Form form;
  check(htmlform::parseFirstForm(page, std::strlen(page), form), "uppercase form parses");
  check(form.post, "uppercase POST detected");
  const char* want[][2] = {{"plan", "premium"}, {"msg", "hi & bye"}, {"ok", "Go online"}};
  expectFields(form, want, 3, "uppercase form fields (disabled skipped, checked radio kept)");

  char url[htmlform::kMaxUrl], body[htmlform::kMaxBody];
  check(htmlform::buildRequest(form, "http://h/x/y", url, sizeof(url), body, sizeof(body)), "uppercase builds");
  check(std::strcmp(url, "http://h/a/b") == 0, "uppercase action resolves host-relative");
  check(std::strcmp(body, "plan=premium&msg=hi+%26+bye&ok=Go+online") == 0, "uppercase body encodes");
}

// Numeric entities and encoding of reserved characters.
void testEntitiesAndEncoding() {
  const char* page = "<form method=post>"
                     "<input type=hidden name=t value=\"&#65;&#x42;&amp;\">"
                     "<input name=q value=\"a b&c=d\">"
                     "</form>";
  htmlform::Form form;
  check(htmlform::parseFirstForm(page, std::strlen(page), form), "entities parse");
  const char* want[][2] = {{"t", "AB&"}, {"q", "a b&c=d"}};
  expectFields(form, want, 2, "entities decoded");

  char url[htmlform::kMaxUrl], body[htmlform::kMaxBody];
  check(htmlform::buildRequest(form, "http://h/", url, sizeof(url), body, sizeof(body)), "entities build");
  check(std::strcmp(body, "t=AB%26&q=a+b%26c%3Dd") == 0, "body urlencodes reserved chars");

  char enc[32];
  check(htmlform::encodeValue("a b&c=d", enc, sizeof(enc)) && std::strcmp(enc, "a+b%26c%3Dd") == 0, "encodeValue");
  check(htmlform::encodeValue("~-.*_zZ9", enc, sizeof(enc)) && std::strcmp(enc, "~-.*_zZ9") == 0,
        "encodeValue unreserved passthrough");
  check(!htmlform::encodeValue("abcd", enc, 4), "encodeValue overflow detected"); // 'a','b','c' + NUL
}

// URL resolution rules (forms built directly: resolution is the subject).
void testResolution() {
  const char* page = "http://h/a/b/c?x=1";
  char url[htmlform::kMaxUrl], body[htmlform::kMaxBody];

  auto checkResolve = [&](const char* action, bool post, const char* wantUrl, const char* what) {
    htmlform::Form form;
    std::snprintf(form.action, sizeof(form.action), "%s", action);
    form.post = post;
    const bool built = htmlform::buildRequest(form, page, url, sizeof(url), body, sizeof(body));
    if (!built || std::strcmp(url, wantUrl) != 0) {
      std::printf("FAIL: %s (got %s)\n", what, built ? url : "(no build)");
      ++failures;
    }
  };

  checkResolve("d.html", false, "http://h/a/b/d.html", "relative GET drops page query");
  checkResolve("/e", true, "http://h/e", "host-relative action");
  checkResolve("//h2/f", true, "http://h2/f", "scheme-relative gains http:");
  checkResolve("http://h3/g", false, "http://h3/g", "absolute http passes through");
  checkResolve("?y=2", true, "http://h/a/b/c?y=2", "query-only action keeps path");
  checkResolve("./d2", true, "http://h/a/b/d2", "leading ./ tolerated");
  checkResolve("", true, "http://h/a/b/c?x=1", "empty action keeps page query (POST)");
  checkResolve("frag#f", true, "http://h/a/b/frag", "fragment stripped");

  htmlform::Form https;
  std::strcpy(https.action, "https://x/y");
  check(!htmlform::buildRequest(https, page, url, sizeof(url), body, sizeof(body)), "https action rejected");
  htmlform::Form js;
  std::strcpy(js.action, "javascript:alert(1)");
  check(!htmlform::buildRequest(js, page, url, sizeof(url), body, sizeof(body)), "javascript action rejected");
  check(!htmlform::resolveUrl("x", "https://secure/page", url, sizeof(url)), "https page url rejected");
  check(!htmlform::resolveUrl("x", "not a url", url, sizeof(url)), "malformed page url rejected");
}

// Malformed and degenerate pages.
void testMalformed() {
  htmlform::Form form;
  const char* noform = "<html><p>nope</p></html>";
  check(!htmlform::parseFirstForm(noform, std::strlen(noform), form), "no form returns false");
  check(!htmlform::parseFirstForm("", 0, form), "empty document returns false");
  const char* lookalike = "<formx action=/a>";
  check(!htmlform::parseFirstForm(lookalike, std::strlen(lookalike), form), "lookalike tag ignored");

  const char* unclosed = "<form action=/a><input name=a value=1>";
  check(htmlform::parseFirstForm(unclosed, std::strlen(unclosed), form), "unclosed form parses leniently");
  const char* want[][2] = {{"a", "1"}};
  expectFields(form, want, 1, "unclosed form fields");

  const char* nested = "<div>text<form><input name=a value=1></form><input name=b value=2>";
  check(htmlform::parseFirstForm(nested, std::strlen(nested), form), "bounded region parses");
  expectFields(form, want, 1, "fields after </form> ignored");

  // Field overflow: 14 inputs keep the first 12.
  char big[512] = "<form>";
  for (int i = 0; i < 14; ++i)
    std::snprintf(big + std::strlen(big), sizeof(big) - std::strlen(big), "<input name=f%d value=%d>", i, i);
  std::strncat(big, "</form>", sizeof(big) - std::strlen(big));
  check(htmlform::parseFirstForm(big, std::strlen(big), form), "overflow form parses");
  check(form.fieldCount == htmlform::kMaxFields, "field overflow caps at kMaxFields");
  check(std::strcmp(form.fields[0].name, "f0") == 0 && std::strcmp(form.fields[11].name, "f11") == 0,
        "kept fields are the first ones");

  // Radio group with nothing checked contributes no field.
  const char* radio = "<form method=post><input type=radio name=g value=1><input type=radio name=g value=2></form>";
  check(htmlform::parseFirstForm(radio, std::strlen(radio), form), "radio page parses");
  check(form.fieldCount == 0, "unchecked radios contribute nothing");

  // A <select> is out of scope and contributes nothing.
  const char* select = "<form method=post><select name=s><option value=1>one</option></select></form>";
  check(htmlform::parseFirstForm(select, std::strlen(select), form), "select page parses");
  check(form.fieldCount == 0, "select contributes nothing");
}

} // namespace

int main() {
  testTrainPortal();
  testCheckboxGet();
  testUppercaseMixed();
  testEntitiesAndEncoding();
  testResolution();
  testMalformed();

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all htmlform checks passed\n");
  return EXIT_SUCCESS;
}
