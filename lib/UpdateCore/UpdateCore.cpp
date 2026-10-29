#include "UpdateCore.h"
#include <cstring>

namespace updatecore {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isHex(char c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
bool isAlnum(char c) { return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Digits only, at most 9 of them (fits an int comfortably).
bool parseNum(const char*& p, int& out) {
    if (!isDigit(*p)) return false;
    long v = 0;
    int digits = 0;
    while (isDigit(*p)) {
        v = v * 10 + (*p - '0');
        if (++digits > 9) return false;
        ++p;
    }
    out = static_cast<int>(v);
    return true;
}

bool allDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!isDigit(c)) return false;
    return true;
}

// Compare two all-digit strings numerically without overflow, however long.
int cmpNumericIdent(const std::string& a, const std::string& b) {
    size_t ia = a.find_first_not_of('0'), ib = b.find_first_not_of('0');
    std::string x = ia == std::string::npos ? "" : a.substr(ia);
    std::string y = ib == std::string::npos ? "" : b.substr(ib);
    if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
    int c = x.compare(y);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

std::string nextIdent(const std::string& s, size_t& pos) {
    size_t dot = s.find('.', pos);
    std::string id = s.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
    pos = dot == std::string::npos ? s.size() + 1 : dot + 1;
    return id;
}

// ---- minimal JSON, enough for the flat manifest ---------------------------

const char* skipWs(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}

// p at the opening quote. Returns the position after the closing quote, or
// nullptr if malformed. \uXXXX is accepted but decoded as '?': nothing in the
// manifest needs non-ASCII.
const char* parseString(const char* p, std::string* out) {
    if (*p != '"') return nullptr;
    ++p;
    if (out) out->clear();
    while (*p && *p != '"') {
        unsigned char c = static_cast<unsigned char>(*p++);
        if (c == '\\') {
            char e = *p++;
            char dec = 0;
            switch (e) {
                case '"': dec = '"'; break;
                case '\\': dec = '\\'; break;
                case '/': dec = '/'; break;
                case 'b': dec = '\b'; break;
                case 'f': dec = '\f'; break;
                case 'n': dec = '\n'; break;
                case 'r': dec = '\r'; break;
                case 't': dec = '\t'; break;
                case 'u':
                    for (int i = 0; i < 4; ++i) {
                        if (!isHex(*p)) return nullptr;
                        ++p;
                    }
                    dec = '?';
                    break;
                default: return nullptr;
            }
            if (out) out->push_back(dec);
        } else if (c < 0x20) {
            return nullptr;
        } else if (out) {
            out->push_back(static_cast<char>(c));
        }
    }
    return *p == '"' ? p + 1 : nullptr;
}

// Skip one JSON value of any type. nullptr if malformed.
const char* skipValue(const char* p, int depth) {
    if (depth > 8) return nullptr;
    p = skipWs(p);
    switch (*p) {
        case '"': return parseString(p, nullptr);
        case '{': {
            p = skipWs(p + 1);
            if (*p == '}') return p + 1;
            for (;;) {
                p = skipWs(p);
                p = parseString(p, nullptr);
                if (!p) return nullptr;
                p = skipWs(p);
                if (*p != ':') return nullptr;
                p = skipValue(p + 1, depth + 1);
                if (!p) return nullptr;
                p = skipWs(p);
                if (*p == ',') { ++p; continue; }
                return *p == '}' ? p + 1 : nullptr;
            }
        }
        case '[': {
            p = skipWs(p + 1);
            if (*p == ']') return p + 1;
            for (;;) {
                p = skipValue(p, depth + 1);
                if (!p) return nullptr;
                p = skipWs(p);
                if (*p == ',') { ++p; continue; }
                return *p == ']' ? p + 1 : nullptr;
            }
        }
        default: {
            // number / true / false / null
            if (!(isDigit(*p) || *p == '-' || *p == 't' || *p == 'f' || *p == 'n')) return nullptr;
            const char* start = p;
            while (*p && !strchr(",}] \t\r\n", *p)) ++p;
            return p == start ? nullptr : p;
        }
    }
}

// A plain non-negative integer (what the manifest uses for schema and size).
// False if the value isn't one - the caller then treats the field as absent.
bool parseUint(const char*& p, uint64_t& out) {
    if (!isDigit(*p)) return false;
    uint64_t v = 0;
    while (isDigit(*p)) {
        v = v * 10 + static_cast<uint64_t>(*p - '0');
        if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull + 1;  // clamp, keep scanning
        ++p;
    }
    if (*p && !strchr(",} \t\r\n", *p)) return false;  // 1.5, 1e3 ...
    out = v;
    return true;
}

}  // namespace

bool parseVersion(const char* s, Version& out) {
    if (!s) return false;
    const char* p = s;
    Version v;
    if (!parseNum(p, v.major) || *p++ != '.') return false;
    if (!parseNum(p, v.minor) || *p++ != '.') return false;
    if (!parseNum(p, v.patch)) return false;
    if (*p == '-') {
        ++p;
        const char* start = p;
        while (isAlnum(*p) || *p == '.' || *p == '-') ++p;
        if (p == start) return false;
        v.pre.assign(start, p - start);
        if (v.pre.front() == '.' || v.pre.back() == '.' || v.pre.find("..") != std::string::npos)
            return false;
    }
    if (*p) return false;
    out = v;
    return true;
}

int compareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre.empty() && b.pre.empty()) return 0;
    if (a.pre.empty()) return 1;  // a release outranks any of its prereleases
    if (b.pre.empty()) return -1;

    size_t pa = 0, pb = 0;
    while (pa <= a.pre.size() && pb <= b.pre.size()) {
        std::string ia = nextIdent(a.pre, pa), ib = nextIdent(b.pre, pb);
        bool da = allDigits(ia), db = allDigits(ib);
        if (da && db) {
            int c = cmpNumericIdent(ia, ib);
            if (c) return c;
        } else if (da) {
            return -1;  // numeric identifiers sort before alphanumeric ones
        } else if (db) {
            return 1;
        } else {
            int c = ia.compare(ib);
            if (c) return c < 0 ? -1 : 1;
        }
    }
    bool moreA = pa <= a.pre.size(), moreB = pb <= b.pre.size();
    if (moreA != moreB) return moreA ? 1 : -1;  // the longer identifier list wins
    return 0;
}

ManifestError parseManifest(const char* json, Manifest& out) {
    if (!json) return ManifestError::Malformed;
    Manifest m;
    bool haveSchema = false, haveVersion = false, haveUrl = false, haveSha = false,
         haveSize = false, haveMin = false;
    uint64_t schema = 0, size = 0;

    const char* p = skipWs(json);
    if (*p != '{') return ManifestError::Malformed;
    p = skipWs(p + 1);
    if (*p == '}') {
        ++p;
    } else {
        for (;;) {
            std::string key;
            p = parseString(skipWs(p), &key);
            if (!p) return ManifestError::Malformed;
            p = skipWs(p);
            if (*p != ':') return ManifestError::Malformed;
            p = skipWs(p + 1);

            std::string* strDst = nullptr;
            bool* strHave = nullptr;
            uint64_t* numDst = nullptr;
            bool* numHave = nullptr;
            if (key == "version") { strDst = &m.version; strHave = &haveVersion; }
            else if (key == "min_version") { strDst = &m.minVersion; strHave = &haveMin; }
            else if (key == "url") { strDst = &m.url; strHave = &haveUrl; }
            else if (key == "sha256") { strDst = &m.sha256; strHave = &haveSha; }
            else if (key == "notes") { strDst = &m.notes; }
            else if (key == "schema") { numDst = &schema; numHave = &haveSchema; }
            else if (key == "size") { numDst = &size; numHave = &haveSize; }

            if (strDst && *p == '"') {
                p = parseString(p, strDst);
                if (!p) return ManifestError::Malformed;
                if (strHave) *strHave = true;
            } else if (numDst && isDigit(*p)) {
                const char* q = p;
                uint64_t v = 0;
                if (parseUint(q, v)) {
                    *numDst = v;
                    *numHave = true;
                    p = q;
                } else {
                    p = skipValue(p, 0);  // present but not a plain integer
                    if (!p) return ManifestError::Malformed;
                }
            } else {
                // unknown key, or a known key of the wrong type: skip it (a
                // wrong-typed required field is then reported as missing)
                p = skipValue(p, 0);
                if (!p) return ManifestError::Malformed;
            }

            p = skipWs(p);
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; break; }
            return ManifestError::Malformed;
        }
    }
    if (*skipWs(p)) return ManifestError::Malformed;  // trailing garbage

    // Schema first: a future, differently-shaped manifest must read as "too
    // new for this firmware", not as a confusing missing field.
    if (!haveSchema) return ManifestError::MissingField;
    if (schema != 1) return ManifestError::UnsupportedSchema;

    if (!haveVersion || !haveUrl || !haveSha || !haveSize) return ManifestError::MissingField;
    Version v;
    if (!parseVersion(m.version.c_str(), v)) return ManifestError::MissingField;
    if (haveMin && !parseVersion(m.minVersion.c_str(), v)) return ManifestError::MissingField;
    if (m.url.compare(0, 8, "https://") != 0 || m.url.size() <= 8) return ManifestError::BadUrl;
    if (m.sha256.size() != 64) return ManifestError::BadHash;
    for (char& c : m.sha256) {
        if (!isHex(c)) return ManifestError::BadHash;
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }
    if (size == 0 || size > 0x7FFFFFFFull) return ManifestError::MissingField;

    m.schema = static_cast<int>(schema);
    m.size = static_cast<uint32_t>(size);
    out = m;
    return ManifestError::None;
}

Decision decide(const char* running, const Manifest& m) {
    Version man;
    if (!parseVersion(m.version.c_str(), man)) return Decision::UpToDate;  // never offer garbage

    Version run;
    if (!parseVersion(running, run)) {
        // development build: offer whatever is published, unless it's literally this
        return (running && m.version == running) ? Decision::UpToDate : Decision::Available;
    }

    Version min;
    if (parseVersion(m.minVersion.c_str(), min) && compareVersions(run, min) < 0)
        return Decision::NeedsUsbInstall;
    return compareVersions(man, run) > 0 ? Decision::Available : Decision::UpToDate;
}

bool batteryAllowsUpdate(int pct, bool externalPower) {
    return externalPower || pct < 0 || pct >= kMinBatteryPct;
}

std::string htmlEscape(const char* s) {
    std::string out;
    if (!s) return out;
    for (; *s; ++s) {
        switch (*s) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(*s);
        }
    }
    return out;
}

std::string makePassword(const uint8_t* random, size_t n) {
    static const char kAlphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";  // 31 symbols
    constexpr size_t kLen = sizeof(kAlphabet) - 1;
    constexpr unsigned kLimit = 256 - (256 % kLen);  // 248: reject the biased tail
    std::string out;
    for (size_t i = 0; i < n && out.size() < kPasswordLen; ++i) {
        if (random[i] < kLimit) out.push_back(kAlphabet[random[i] % kLen]);
    }
    return out;
}

}  // namespace updatecore
