//! Pure logic behind the on-watch firmware update, ported from PuttingWatch's lib/UpdateCore:
//! version ordering, parsing the release manifest, deciding whether to offer an update, and small
//! helpers for the Wi-Fi setup portal. No ESP dependencies, so it is unit-tested on the host.
//!
//! The manifest is a contract with firmware already in the field (see PuttingWatch's
//! tools/make_release_site.py): fields are only ever added, never renamed, and unknown fields are
//! ignored here for exactly that reason.

use serde_json::Value;

// --- versions ----------------------------------------------------------------------------------

/// MAJOR.MINOR.PATCH with an optional -prerelease, e.g. "1.2.3", "1.2.3-beta.1".
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Version {
    pub major: u32,
    pub minor: u32,
    pub patch: u32,
    /// Empty for a release.
    pub pre: String,
}

/// Parse a run of at most 9 digits (fits comfortably in a u32).
fn parse_num(s: &[u8], pos: &mut usize) -> Option<u32> {
    let start = *pos;
    let mut v: u32 = 0;
    while *pos < s.len() && s[*pos].is_ascii_digit() {
        if *pos - start >= 9 {
            return None;
        }
        v = v * 10 + (s[*pos] - b'0') as u32;
        *pos += 1;
    }
    (*pos > start).then_some(v)
}

/// None for anything that isn't strictly that shape. Development builds report strings like
/// "0.1.0-3-gabc1234-dirty", which do parse (as prereleases of the last tag); a bare hash or
/// "dev" does not.
pub fn parse_version(s: &str) -> Option<Version> {
    let b = s.as_bytes();
    let mut p = 0;
    let major = parse_num(b, &mut p)?;
    if b.get(p) != Some(&b'.') {
        return None;
    }
    p += 1;
    let minor = parse_num(b, &mut p)?;
    if b.get(p) != Some(&b'.') {
        return None;
    }
    p += 1;
    let patch = parse_num(b, &mut p)?;
    let mut pre = String::new();
    if b.get(p) == Some(&b'-') {
        p += 1;
        let start = p;
        while p < b.len() && (b[p].is_ascii_alphanumeric() || b[p] == b'.' || b[p] == b'-') {
            p += 1;
        }
        if p == start {
            return None;
        }
        pre = s[start..p].to_string();
        if pre.starts_with('.') || pre.ends_with('.') || pre.contains("..") {
            return None;
        }
    }
    (p == b.len()).then_some(Version { major, minor, patch, pre })
}

fn all_digits(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|c| c.is_ascii_digit())
}

/// Compare two all-digit strings numerically without overflow, however long.
fn cmp_numeric_ident(a: &str, b: &str) -> core::cmp::Ordering {
    let (a, b) = (a.trim_start_matches('0'), b.trim_start_matches('0'));
    a.len().cmp(&b.len()).then_with(|| a.cmp(b))
}

/// Next dot-separated identifier at `*pos`; advances `*pos` past the dot (to len + 1 at the end).
fn next_ident<'a>(s: &'a str, pos: &mut usize) -> &'a str {
    let rest = &s[*pos..];
    match rest.find('.') {
        Some(d) => {
            *pos += d + 1;
            &rest[..d]
        }
        None => {
            *pos = s.len() + 1;
            rest
        }
    }
}

/// SemVer 2.0 precedence. A prerelease sorts before its release.
pub fn compare_versions(a: &Version, b: &Version) -> core::cmp::Ordering {
    use core::cmp::Ordering::*;
    let core = (a.major, a.minor, a.patch).cmp(&(b.major, b.minor, b.patch));
    if core != Equal {
        return core;
    }
    match (a.pre.is_empty(), b.pre.is_empty()) {
        (true, true) => return Equal,
        (true, false) => return Greater, // a release outranks any of its prereleases
        (false, true) => return Less,
        _ => {}
    }
    let (mut pa, mut pb) = (0, 0);
    while pa <= a.pre.len() && pb <= b.pre.len() {
        let (ia, ib) = (next_ident(&a.pre, &mut pa), next_ident(&b.pre, &mut pb));
        match (all_digits(ia), all_digits(ib)) {
            (true, true) => {
                let c = cmp_numeric_ident(ia, ib);
                if c != Equal {
                    return c;
                }
            }
            (true, false) => return Less, // numeric identifiers sort before alphanumeric ones
            (false, true) => return Greater,
            (false, false) => {
                let c = ia.cmp(ib);
                if c != Equal {
                    return c;
                }
            }
        }
    }
    // One list ran out first: the longer identifier list wins.
    match ((pa <= a.pre.len()), (pb <= b.pre.len())) {
        (x, y) if x == y => Equal,
        (true, _) => Greater,
        _ => Less,
    }
}

// --- manifest -----------------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Manifest {
    pub schema: u32,
    pub version: String,
    /// Oldest installed version allowed to update over the air.
    pub min_version: String,
    pub url: String,
    /// 64 lowercase hex chars.
    pub sha256: String,
    pub notes: String,
    pub size: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ManifestError {
    /// Not valid JSON (or not an object).
    Malformed,
    /// Written for a newer updater than this firmware understands.
    UnsupportedSchema,
    /// A required field is absent or has the wrong type.
    MissingField,
    /// Not https://.
    BadUrl,
    /// sha256 not 64 hex chars.
    BadHash,
}

fn string_field(o: &serde_json::Map<String, Value>, key: &str) -> Option<String> {
    o.get(key)?.as_str().map(str::to_string)
}

/// Parse and validate a release manifest.
///
/// The notes text is shown in a 1-bit Latin font, so any non-ASCII character becomes '?' (as the
/// C++ parser did for \u escapes).
pub fn parse_manifest(json: &str) -> Result<Manifest, ManifestError> {
    let Value::Object(o) = serde_json::from_str::<Value>(json).map_err(|_| ManifestError::Malformed)? else {
        return Err(ManifestError::Malformed);
    };

    // Schema first: a future, differently-shaped manifest must read as "too new for this
    // firmware", not as a confusing missing field. as_u64 accepts only plain non-negative integers
    // (not 1.5, 1e3 or -1), which is exactly what the manifest uses.
    let schema = o.get("schema").and_then(Value::as_u64).ok_or(ManifestError::MissingField)?;
    if schema != 1 {
        return Err(ManifestError::UnsupportedSchema);
    }

    let version = string_field(&o, "version");
    let url = string_field(&o, "url");
    let sha = string_field(&o, "sha256");
    let size = o.get("size").and_then(Value::as_u64);
    let (Some(version), Some(url), Some(mut sha256), Some(size)) = (version, url, sha, size) else {
        return Err(ManifestError::MissingField);
    };
    parse_version(&version).ok_or(ManifestError::MissingField)?;
    let min_version = match string_field(&o, "min_version") {
        Some(m) => {
            parse_version(&m).ok_or(ManifestError::MissingField)?;
            m
        }
        None => "0.0.0".to_string(),
    };
    if !url.starts_with("https://") || url.len() <= 8 {
        return Err(ManifestError::BadUrl);
    }
    if sha256.len() != 64 || !sha256.bytes().all(|c| c.is_ascii_hexdigit()) {
        return Err(ManifestError::BadHash);
    }
    sha256.make_ascii_lowercase();
    if size == 0 || size > 0x7FFF_FFFF {
        return Err(ManifestError::MissingField);
    }

    let notes: String = string_field(&o, "notes")
        .unwrap_or_default()
        .chars()
        .map(|c| if c.is_ascii() { c } else { '?' })
        .collect();

    Ok(Manifest { schema: 1, version, min_version, url, sha256, notes, size: size as u32 })
}

// --- deciding ------------------------------------------------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Decision {
    UpToDate,
    Available,
    /// The running firmware is older than `manifest.min_version`.
    NeedsUsbInstall,
}

/// `running` is the firmware's own version string. One that doesn't parse is a development build:
/// any published version is offered (and the min_version gate is skipped) unless it is textually
/// identical.
pub fn decide(running: &str, m: &Manifest) -> Decision {
    let Some(manifest_version) = parse_version(&m.version) else {
        return Decision::UpToDate; // never offer garbage
    };
    let Some(run) = parse_version(running) else {
        return if m.version == running { Decision::UpToDate } else { Decision::Available };
    };
    if let Some(min) = parse_version(&m.min_version) {
        if compare_versions(&run, &min).is_lt() {
            return Decision::NeedsUsbInstall;
        }
    }
    if compare_versions(&manifest_version, &run).is_gt() {
        Decision::Available
    } else {
        Decision::UpToDate
    }
}

/// Don't start flashing on a nearly flat battery.
pub const MIN_BATTERY_PCT: i32 = 40;

/// External power (USB / a charger) always allows it; so does an unknown level (no battery
/// fitted), since the watch can then only be running from USB.
pub fn battery_allows_update(pct: i32, external_power: bool) -> bool {
    external_power || pct < 0 || pct >= MIN_BATTERY_PCT
}

// --- Wi-Fi setup portal helpers ----------------------------------------------------------------

/// Escape text for safe inclusion in HTML (network names are attacker-controlled input).
pub fn html_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    for c in s.chars() {
        match c {
            '&' => out.push_str("&amp;"),
            '<' => out.push_str("&lt;"),
            '>' => out.push_str("&gt;"),
            '"' => out.push_str("&quot;"),
            '\'' => out.push_str("&#39;"),
            c => out.push(c),
        }
    }
    out
}

pub const PASSWORD_LEN: usize = 10;

/// Per-session Wi-Fi password for the watch's setup access point, carried in the QR code.
/// Upper-case letters and digits with the look-alikes (I L O 0 1) removed, so it can also be
/// typed. Consumes the supplied random bytes, discarding those that would bias the choice; returns
/// up to `PASSWORD_LEN` characters, so give it plenty more random bytes than that (e.g. 64).
pub fn make_password(random: &[u8]) -> String {
    const ALPHABET: &[u8] = b"ABCDEFGHJKMNPQRSTUVWXYZ23456789"; // 31 symbols
    const LIMIT: u8 = (256 - 256 % ALPHABET.len()) as u8; // 248: reject the biased tail
    let mut out = String::new();
    for &r in random {
        if out.len() >= PASSWORD_LEN {
            break;
        }
        if r < LIMIT {
            out.push(ALPHABET[r as usize % ALPHABET.len()] as char);
        }
    }
    out
}
