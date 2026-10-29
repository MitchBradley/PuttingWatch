// Host-side unit tests for lib/UpdateCore - the pure logic behind the on-watch
// firmware update.
//   pio test -e native

#include <unity.h>
#include <cstring>
#include "UpdateCore.h"

using namespace updatecore;

static int cmp(const char* a, const char* b) {
    Version va, vb;
    TEST_ASSERT_TRUE_MESSAGE(parseVersion(a, va), a);
    TEST_ASSERT_TRUE_MESSAGE(parseVersion(b, vb), b);
    return compareVersions(va, vb);
}

// ---- versions -------------------------------------------------------------

void test_parse_version_accepts(void) {
    Version v;
    TEST_ASSERT_TRUE(parseVersion("1.2.3", v));
    TEST_ASSERT_EQUAL_INT(1, v.major);
    TEST_ASSERT_EQUAL_INT(2, v.minor);
    TEST_ASSERT_EQUAL_INT(3, v.patch);
    TEST_ASSERT_EQUAL_STRING("", v.pre.c_str());

    TEST_ASSERT_TRUE(parseVersion("0.1.0-beta.1", v));
    TEST_ASSERT_EQUAL_STRING("beta.1", v.pre.c_str());

    // What `git describe` gives between tags / with uncommitted changes: a
    // valid (if odd) prerelease, so it still has a defined ordering.
    TEST_ASSERT_TRUE(parseVersion("0.1.0-3-gabc1234", v));
    TEST_ASSERT_TRUE(parseVersion("0.1.0-3-gabc1234-dirty", v));
    TEST_ASSERT_TRUE(parseVersion("10.20.30", v));
}

void test_parse_version_rejects(void) {
    Version v;
    const char* bad[] = {"",       "1.2",      "v1.2.3",  "1.2.3.4",       "1.2.x",  "dev",
                         "b3793a2", "unknown", "1.2.3-",  "1.2.3-a..b",    "1.2.3-.a", "1.2.3-a.",
                         "1.2.3+build", " 1.2.3", "1.2.3 ", "1.-2.3",      "1234567890.0.0"};
    for (const char* s : bad) TEST_ASSERT_FALSE_MESSAGE(parseVersion(s, v), s);
    TEST_ASSERT_FALSE(parseVersion(nullptr, v));
}

void test_compare_core_numbers(void) {
    TEST_ASSERT_EQUAL_INT(0, cmp("1.2.3", "1.2.3"));
    TEST_ASSERT_TRUE(cmp("1.2.3", "1.2.4") < 0);
    TEST_ASSERT_TRUE(cmp("1.2.9", "1.3.0") < 0);
    TEST_ASSERT_TRUE(cmp("1.99.99", "2.0.0") < 0);
    TEST_ASSERT_TRUE(cmp("0.10.0", "0.9.0") > 0);  // numeric, not lexical
}

// The precedence chain from the SemVer 2.0 spec, section 11.
void test_compare_semver_spec_chain(void) {
    const char* chain[] = {"1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                           "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0"};
    const int n = sizeof(chain) / sizeof(chain[0]);
    for (int i = 0; i + 1 < n; i++) {
        TEST_ASSERT_TRUE_MESSAGE(cmp(chain[i], chain[i + 1]) < 0, chain[i]);
        TEST_ASSERT_TRUE_MESSAGE(cmp(chain[i + 1], chain[i]) > 0, chain[i + 1]);
    }
}

void test_compare_prerelease_vs_neighbours(void) {
    TEST_ASSERT_TRUE(cmp("0.9.9", "1.0.0-alpha") < 0);
    TEST_ASSERT_TRUE(cmp("1.0.0-rc.1", "1.0.1") < 0);
    // development builds sort below the release they were cut after
    TEST_ASSERT_TRUE(cmp("0.1.0-3-gabc1234", "0.1.0") < 0);
}

// ---- manifest -------------------------------------------------------------

// Exactly what tools/make_release_site.py emits.
static const char* kGoodManifest = R"({
  "schema": 1,
  "version": "0.1.0",
  "min_version": "0.0.0",
  "released": "2026-10-29T16:47:21Z",
  "url": "https://mitchbradley.github.io/PuttingWatch/firmware/0.1.0/firmware.bin",
  "size": 2027184,
  "sha256": "db4d3ec930a1330ac9585a5667a34810be7f6885d979a301d13b8d3da0557c9b",
  "notes": "First beta"
}
)";

void test_manifest_parses_generator_output(void) {
    Manifest m;
    TEST_ASSERT_EQUAL_INT((int)ManifestError::None, (int)parseManifest(kGoodManifest, m));
    TEST_ASSERT_EQUAL_INT(1, m.schema);
    TEST_ASSERT_EQUAL_STRING("0.1.0", m.version.c_str());
    TEST_ASSERT_EQUAL_STRING("0.0.0", m.minVersion.c_str());
    TEST_ASSERT_EQUAL_STRING("https://mitchbradley.github.io/PuttingWatch/firmware/0.1.0/firmware.bin",
                             m.url.c_str());
    TEST_ASSERT_EQUAL_UINT32(2027184, m.size);
    TEST_ASSERT_EQUAL_STRING("db4d3ec930a1330ac9585a5667a34810be7f6885d979a301d13b8d3da0557c9b",
                             m.sha256.c_str());
    TEST_ASSERT_EQUAL_STRING("First beta", m.notes.c_str());
}

void test_manifest_compact_and_reordered(void) {
    Manifest m;
    const char* j =
        R"({"size":100,"sha256":"DB4D3EC930A1330AC9585A5667A34810BE7F6885D979A301D13B8D3DA0557C9B",)"
        R"("url":"https://x.example/f.bin","version":"1.0.0","schema":1})";
    TEST_ASSERT_EQUAL_INT((int)ManifestError::None, (int)parseManifest(j, m));
    TEST_ASSERT_EQUAL_UINT32(100, m.size);
    // upper-case hash is normalised to lower case; min_version defaults
    TEST_ASSERT_EQUAL_STRING("db4d3ec930a1330ac9585a5667a34810be7f6885d979a301d13b8d3da0557c9b",
                             m.sha256.c_str());
    TEST_ASSERT_EQUAL_STRING("0.0.0", m.minVersion.c_str());
    TEST_ASSERT_EQUAL_STRING("", m.notes.c_str());
}

// Fields get added in later releases; an older watch has to ignore them,
// whatever shape they take.
void test_manifest_ignores_unknown_fields(void) {
    Manifest m;
    const char* j =
        R"({"schema":1,"future":{"a":[1,2,{"b":"}"}],"c":null},"flag":true,"n":-1.5e3,)"
        R"("version":"2.0.0","url":"https://x/y.bin","size":5,)"
        R"("sha256":"db4d3ec930a1330ac9585a5667a34810be7f6885d979a301d13b8d3da0557c9b","list":[]})";
    TEST_ASSERT_EQUAL_INT((int)ManifestError::None, (int)parseManifest(j, m));
    TEST_ASSERT_EQUAL_STRING("2.0.0", m.version.c_str());
}

void test_manifest_string_escapes(void) {
    Manifest m;
    const char* j =
        R"({"schema":1,"version":"1.0.0","url":"https://x/y.bin","size":5,)"
        R"("sha256":"db4d3ec930a1330ac9585a5667a34810be7f6885d979a301d13b8d3da0557c9b",)"
        // The \u escape goes through a normal literal ("\\u" = backslash, u):
        // the compiler converts a \uXXXX inside a raw string to the real
        // character, so the parser would never see the escape.
        R"("notes":"say \"hi\"\\ \n caf)" "\\u00e9" R"("})";
    TEST_ASSERT_EQUAL_INT((int)ManifestError::None, (int)parseManifest(j, m));
    TEST_ASSERT_EQUAL_STRING("say \"hi\"\\ \n caf?", m.notes.c_str());
}

void test_manifest_errors(void) {
    Manifest m;
    const char* sha = R"("sha256":"db4d3ec930a1330ac9585a5667a34810be7f6885d979a301d13b8d3da0557c9b")";
    char buf[512];
    auto with = [&](const char* body) {
        snprintf(buf, sizeof(buf), body, sha);
        return parseManifest(buf, m);
    };

    // syntax
    TEST_ASSERT_EQUAL_INT((int)ManifestError::Malformed, (int)parseManifest("", m));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::Malformed, (int)parseManifest("[]", m));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::Malformed, (int)parseManifest("<html>404</html>", m));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::Malformed, (int)parseManifest(nullptr, m));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::Malformed, (int)parseManifest(R"({"schema":1,"version":"1.0.0")", m));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::Malformed, (int)parseManifest(R"({"schema":1,})" "x", m));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::Malformed, (int)parseManifest(R"({"schema":1} trailing)", m));
    // a truncated download must never parse as something valid
    for (size_t n = 1; n + 1 < strlen(kGoodManifest); n += 7) {
        std::string cut(kGoodManifest, n);
        TEST_ASSERT_NOT_EQUAL((int)ManifestError::None, (int)parseManifest(cut.c_str(), m));
    }

    // schema
    TEST_ASSERT_EQUAL_INT((int)ManifestError::MissingField,
                          (int)with(R"({"version":"1.0.0","url":"https://x/y","size":5,%s})"));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::UnsupportedSchema,
                          (int)with(R"({"schema":2,"version":"1.0.0","url":"https://x/y","size":5,%s})"));
    // a future schema that changes field types is still "too new", not "missing"
    TEST_ASSERT_EQUAL_INT((int)ManifestError::UnsupportedSchema,
                          (int)parseManifest(R"({"schema":2,"version":7,"url":[]})", m));

    // required fields
    TEST_ASSERT_EQUAL_INT((int)ManifestError::MissingField,
                          (int)with(R"({"schema":1,"url":"https://x/y","size":5,%s})"));  // no version
    TEST_ASSERT_EQUAL_INT((int)ManifestError::MissingField,
                          (int)with(R"({"schema":1,"version":"1.0.0","size":5,%s})"));  // no url
    TEST_ASSERT_EQUAL_INT((int)ManifestError::MissingField,
                          (int)with(R"({"schema":1,"version":"1.0.0","url":"https://x/y","size":5})"));  // no sha
    TEST_ASSERT_EQUAL_INT((int)ManifestError::MissingField,
                          (int)with(R"({"schema":1,"version":"1.0.0","url":"https://x/y",%s})"));  // no size
    TEST_ASSERT_EQUAL_INT((int)ManifestError::MissingField,
                          (int)with(R"({"schema":1,"version":"1.0.0","url":"https://x/y","size":"5",%s})"));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::MissingField,
                          (int)with(R"({"schema":1,"version":"1.0.0","url":"https://x/y","size":0,%s})"));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::MissingField,
                          (int)with(R"({"schema":1,"version":"1.0.0","url":"https://x/y","size":-5,%s})"));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::MissingField,
                          (int)with(R"({"schema":1,"version":"1.0.0","url":"https://x/y","size":99999999999,%s})"));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::MissingField,
                          (int)with(R"({"schema":1,"version":"banana","url":"https://x/y","size":5,%s})"));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::MissingField,
                          (int)with(R"({"schema":1,"version":"1.0.0","min_version":"x","url":"https://x/y","size":5,%s})"));

    // url / hash
    TEST_ASSERT_EQUAL_INT((int)ManifestError::BadUrl,
                          (int)with(R"({"schema":1,"version":"1.0.0","url":"http://x/y","size":5,%s})"));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::BadUrl,
                          (int)with(R"({"schema":1,"version":"1.0.0","url":"https://","size":5,%s})"));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::BadUrl,
                          (int)with(R"({"schema":1,"version":"1.0.0","url":"ftp://x/y","size":5,%s})"));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::BadHash,
                          (int)parseManifest(R"({"schema":1,"version":"1.0.0","url":"https://x/y","size":5,"sha256":"abc"})", m));
    TEST_ASSERT_EQUAL_INT((int)ManifestError::BadHash,
                          (int)parseManifest(R"({"schema":1,"version":"1.0.0","url":"https://x/y","size":5,)"
                                             R"("sha256":"zb4d3ec930a1330ac9585a5667a34810be7f6885d979a301d13b8d3da0557c9b"})", m));
}

// ---- decision -------------------------------------------------------------

static Manifest manifest(const char* version, const char* minVersion = "0.0.0") {
    Manifest m;
    m.schema = 1;
    m.version = version;
    m.minVersion = minVersion;
    return m;
}

void test_decide_release_builds(void) {
    TEST_ASSERT_EQUAL_INT((int)Decision::Available, (int)decide("0.1.0", manifest("0.2.0")));
    TEST_ASSERT_EQUAL_INT((int)Decision::UpToDate, (int)decide("0.2.0", manifest("0.2.0")));
    TEST_ASSERT_EQUAL_INT((int)Decision::UpToDate, (int)decide("0.3.0", manifest("0.2.0")));  // never downgrade
    TEST_ASSERT_EQUAL_INT((int)Decision::Available, (int)decide("0.1.0-beta.1", manifest("0.1.0")));
    TEST_ASSERT_EQUAL_INT((int)Decision::Available, (int)decide("0.1.0-beta.1", manifest("0.1.0-beta.2")));
    TEST_ASSERT_EQUAL_INT((int)Decision::UpToDate, (int)decide("0.1.0", manifest("0.1.0-beta.2")));
}

void test_decide_min_version_gate(void) {
    // can't be applied over the air: ask for a USB install instead
    TEST_ASSERT_EQUAL_INT((int)Decision::NeedsUsbInstall, (int)decide("0.1.0", manifest("0.3.0", "0.2.0")));
    TEST_ASSERT_EQUAL_INT((int)Decision::Available, (int)decide("0.2.0", manifest("0.3.0", "0.2.0")));
    TEST_ASSERT_EQUAL_INT((int)Decision::Available, (int)decide("0.2.5", manifest("0.3.0", "0.2.0")));
}

void test_decide_development_builds(void) {
    // not a comparable version: offer whatever is published
    TEST_ASSERT_EQUAL_INT((int)Decision::Available, (int)decide("b3793a2", manifest("0.1.0")));
    TEST_ASSERT_EQUAL_INT((int)Decision::Available, (int)decide("dev", manifest("0.1.0")));
    TEST_ASSERT_EQUAL_INT((int)Decision::Available, (int)decide("unknown", manifest("0.1.0")));
    // ...without applying the USB-only gate to a developer's own build
    TEST_ASSERT_EQUAL_INT((int)Decision::Available, (int)decide("dev", manifest("0.3.0", "0.2.0")));
    // a git-describe string does parse (as a prerelease of the last tag), so
    // it sorts below that release and is offered it
    TEST_ASSERT_EQUAL_INT((int)Decision::Available, (int)decide("0.1.0-3-gabc1234", manifest("0.1.0")));
    TEST_ASSERT_EQUAL_INT((int)Decision::Available, (int)decide("0.1.0-3-gabc1234-dirty", manifest("0.1.0")));
}

void test_decide_never_offers_garbage(void) {
    TEST_ASSERT_EQUAL_INT((int)Decision::UpToDate, (int)decide("0.1.0", manifest("banana")));
}

// ---- helpers --------------------------------------------------------------

void test_battery_gate(void) {
    TEST_ASSERT_FALSE(batteryAllowsUpdate(10, false));
    TEST_ASSERT_FALSE(batteryAllowsUpdate(39, false));
    TEST_ASSERT_TRUE(batteryAllowsUpdate(40, false));
    TEST_ASSERT_TRUE(batteryAllowsUpdate(100, false));
    TEST_ASSERT_TRUE(batteryAllowsUpdate(5, true));   // on a charger: always fine
    TEST_ASSERT_TRUE(batteryAllowsUpdate(-1, false)); // no battery reading: must be on USB
}

void test_html_escape(void) {
    TEST_ASSERT_EQUAL_STRING("plain", htmlEscape("plain").c_str());
    TEST_ASSERT_EQUAL_STRING("&lt;script&gt;alert(&quot;x&quot;)&lt;/script&gt;",
                             htmlEscape("<script>alert(\"x\")</script>").c_str());
    TEST_ASSERT_EQUAL_STRING("Bob&#39;s &amp; Alice&#39;s", htmlEscape("Bob's & Alice's").c_str());
    TEST_ASSERT_EQUAL_STRING("", htmlEscape("").c_str());
    TEST_ASSERT_EQUAL_STRING("", htmlEscape(nullptr).c_str());
}

void test_make_password(void) {
    // deterministic mapping: index 0 -> 'A', 22 -> 'Z', 23 -> '2', 30 -> '9'
    uint8_t zeros[16] = {0};
    TEST_ASSERT_EQUAL_STRING("AAAAAAAAAA", makePassword(zeros, sizeof(zeros)).c_str());
    uint8_t edge[] = {22, 23, 30, 31, 0};  // 31 wraps to index 0
    TEST_ASSERT_EQUAL_STRING("Z29AA", makePassword(edge, sizeof(edge)).c_str());

    // bytes in the biased tail (>= 248) are discarded, not folded in
    uint8_t tail[] = {255, 250, 248, 0, 1};
    TEST_ASSERT_EQUAL_STRING("AB", makePassword(tail, sizeof(tail)).c_str());

    // never longer than kPasswordLen, and only unambiguous characters
    uint8_t all[256];
    for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
    std::string pw = makePassword(all, sizeof(all));
    TEST_ASSERT_EQUAL_UINT(kPasswordLen, pw.size());
    for (char c : pw) {
        TEST_ASSERT_NOT_NULL(strchr("ABCDEFGHJKMNPQRSTUVWXYZ23456789", c));
        TEST_ASSERT_NULL(strchr("ILO01", c));
    }
    TEST_ASSERT_EQUAL_STRING("", makePassword(zeros, 0).c_str());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_version_accepts);
    RUN_TEST(test_parse_version_rejects);
    RUN_TEST(test_compare_core_numbers);
    RUN_TEST(test_compare_semver_spec_chain);
    RUN_TEST(test_compare_prerelease_vs_neighbours);
    RUN_TEST(test_manifest_parses_generator_output);
    RUN_TEST(test_manifest_compact_and_reordered);
    RUN_TEST(test_manifest_ignores_unknown_fields);
    RUN_TEST(test_manifest_string_escapes);
    RUN_TEST(test_manifest_errors);
    RUN_TEST(test_decide_release_builds);
    RUN_TEST(test_decide_min_version_gate);
    RUN_TEST(test_decide_development_builds);
    RUN_TEST(test_decide_never_offers_garbage);
    RUN_TEST(test_battery_gate);
    RUN_TEST(test_html_escape);
    RUN_TEST(test_make_password);
    return UNITY_END();
}
