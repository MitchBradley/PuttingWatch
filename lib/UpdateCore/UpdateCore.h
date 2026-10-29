#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Pure logic behind the on-watch firmware update: version ordering, parsing
// the release manifest, deciding whether to offer an update, and small
// helpers for the Wi-Fi setup portal. No Arduino/ESP dependencies, so it is
// unit-tested on the host (test/test_updatecore).
//
// The manifest is a contract with firmware already in the field - see
// tools/make_release_site.py. Fields are only ever added, never renamed, and
// unknown fields are ignored here for exactly that reason.
namespace updatecore {

// MAJOR.MINOR.PATCH with an optional -prerelease, e.g. "1.2.3", "1.2.3-beta.1".
struct Version {
    int major = 0, minor = 0, patch = 0;
    std::string pre;  // empty for a release
};

// False for anything that isn't strictly that shape - notably the
// "0.1.0-3-gabc1234-dirty" / bare-hash strings development builds report
// (those do parse as prereleases when they start with a version; a bare hash
// or "dev" does not).
bool parseVersion(const char* s, Version& out);

// SemVer 2.0 precedence: <0, 0, >0. A prerelease sorts before its release.
int compareVersions(const Version& a, const Version& b);

struct Manifest {
    int schema = 0;
    std::string version;
    std::string minVersion = "0.0.0";  // oldest installed version allowed to update OTA
    std::string url;
    std::string sha256;                // 64 lowercase hex chars
    std::string notes;
    uint32_t size = 0;
};

enum class ManifestError {
    None,
    Malformed,          // not valid JSON (or not an object)
    UnsupportedSchema,  // written for a newer updater than this firmware understands
    MissingField,       // a required field is absent or has the wrong type
    BadUrl,             // not https://
    BadHash,            // sha256 not 64 hex chars
};

ManifestError parseManifest(const char* json, Manifest& out);

enum class Decision {
    UpToDate,
    Available,
    NeedsUsbInstall,  // running firmware is older than manifest.min_version
};

// `running` is the firmware's own version string. A running version that
// doesn't parse is a development build: any published version is offered
// (and the min_version gate is skipped) unless it is textually identical.
Decision decide(const char* running, const Manifest& m);

// Don't start flashing on a nearly flat battery. External power (USB/charger)
// always allows it; an unknown level (no battery fitted) also does, since the
// watch can only then be running from USB.
constexpr int kMinBatteryPct = 40;
bool batteryAllowsUpdate(int pct, bool externalPower);

// Escape text for safe inclusion in HTML (network names are attacker-
// controlled input in the setup page).
std::string htmlEscape(const char* s);

// Per-session Wi-Fi password for the watch's setup access point, carried in
// the QR code. Upper-case letters and digits with the look-alikes (I L O 0 1)
// removed, so it can also be typed. Consumes the supplied random bytes,
// discarding those that would bias the choice; returns up to kPasswordLen
// characters - give it plenty more random bytes than that (e.g. 64).
constexpr size_t kPasswordLen = 10;
std::string makePassword(const uint8_t* random, size_t n);

}  // namespace updatecore
