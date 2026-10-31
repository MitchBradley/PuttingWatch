//! Port of the C++ firmware's test/test_updatecore (Unity), branch `old-cpp-version`: same cases, same expectations.

use core::cmp::Ordering;
use putting_core::update::*;

fn cmp(a: &str, b: &str) -> Ordering {
    let va = parse_version(a).unwrap_or_else(|| panic!("{a} should parse"));
    let vb = parse_version(b).unwrap_or_else(|| panic!("{b} should parse"));
    compare_versions(&va, &vb)
}

// ---- versions -------------------------------------------------------------

#[test]
fn parse_version_accepts() {
    let v = parse_version("1.2.3").unwrap();
    assert_eq!((v.major, v.minor, v.patch), (1, 2, 3));
    assert_eq!(v.pre, "");

    assert_eq!(parse_version("0.1.0-beta.1").unwrap().pre, "beta.1");

    // What `git describe` gives between tags / with uncommitted changes: a valid (if odd)
    // prerelease, so it still has a defined ordering.
    assert!(parse_version("0.1.0-3-gabc1234").is_some());
    assert!(parse_version("0.1.0-3-gabc1234-dirty").is_some());
    assert!(parse_version("10.20.30").is_some());
}

#[test]
fn parse_version_rejects() {
    let bad = [
        "", "1.2", "v1.2.3", "1.2.3.4", "1.2.x", "dev", "b3793a2", "unknown", "1.2.3-", "1.2.3-a..b",
        "1.2.3-.a", "1.2.3-a.", "1.2.3+build", " 1.2.3", "1.2.3 ", "1.-2.3", "1234567890.0.0",
    ];
    for s in bad {
        assert!(parse_version(s).is_none(), "{s:?} should not parse");
    }
}

#[test]
fn compare_core_numbers() {
    assert_eq!(cmp("1.2.3", "1.2.3"), Ordering::Equal);
    assert!(cmp("1.2.3", "1.2.4").is_lt());
    assert!(cmp("1.2.9", "1.3.0").is_lt());
    assert!(cmp("1.99.99", "2.0.0").is_lt());
    assert!(cmp("0.10.0", "0.9.0").is_gt()); // numeric, not lexical
}

// The precedence chain from the SemVer 2.0 spec, section 11.
#[test]
fn compare_semver_spec_chain() {
    let chain = [
        "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11",
        "1.0.0-rc.1", "1.0.0",
    ];
    for w in chain.windows(2) {
        assert!(cmp(w[0], w[1]).is_lt(), "{} < {}", w[0], w[1]);
        assert!(cmp(w[1], w[0]).is_gt(), "{} > {}", w[1], w[0]);
    }
}

#[test]
fn compare_prerelease_vs_neighbours() {
    assert!(cmp("0.9.9", "1.0.0-alpha").is_lt());
    assert!(cmp("1.0.0-rc.1", "1.0.1").is_lt());
    // development builds sort below the release they were cut after
    assert!(cmp("0.1.0-3-gabc1234", "0.1.0").is_lt());
}

// ---- manifest -------------------------------------------------------------

const SHA: &str = "db4d3ec930a1330ac9585a5667a34810be7f6885d979a301d13b8d3da0557c9b";

// Exactly what tools/make_release_site.py emits.
const GOOD_MANIFEST: &str = r#"{
  "schema": 1,
  "version": "0.1.0",
  "min_version": "0.0.0",
  "released": "2026-10-29T16:47:21Z",
  "url": "https://mitchbradley.github.io/PuttingWatch/firmware/0.1.0/firmware.bin",
  "size": 2027184,
  "sha256": "db4d3ec930a1330ac9585a5667a34810be7f6885d979a301d13b8d3da0557c9b",
  "notes": "First beta"
}
"#;

#[test]
fn manifest_parses_generator_output() {
    let m = parse_manifest(GOOD_MANIFEST).unwrap();
    assert_eq!(m.schema, 1);
    assert_eq!(m.version, "0.1.0");
    assert_eq!(m.min_version, "0.0.0");
    assert_eq!(m.url, "https://mitchbradley.github.io/PuttingWatch/firmware/0.1.0/firmware.bin");
    assert_eq!(m.size, 2027184);
    assert_eq!(m.sha256, SHA);
    assert_eq!(m.notes, "First beta");
}

#[test]
fn manifest_compact_and_reordered() {
    let j = r#"{"size":100,"sha256":"DB4D3EC930A1330AC9585A5667A34810BE7F6885D979A301D13B8D3DA0557C9B","url":"https://x.example/f.bin","version":"1.0.0","schema":1}"#;
    let m = parse_manifest(j).unwrap();
    assert_eq!(m.size, 100);
    // upper-case hash is normalised to lower case; min_version defaults
    assert_eq!(m.sha256, SHA);
    assert_eq!(m.min_version, "0.0.0");
    assert_eq!(m.notes, "");
}

// Fields get added in later releases; an older watch has to ignore them, whatever shape they take.
#[test]
fn manifest_ignores_unknown_fields() {
    let j = format!(
        r#"{{"schema":1,"future":{{"a":[1,2,{{"b":"}}"}}],"c":null}},"flag":true,"n":-1.5e3,"version":"2.0.0","url":"https://x/y.bin","size":5,"sha256":"{SHA}","list":[]}}"#
    );
    assert_eq!(parse_manifest(&j).unwrap().version, "2.0.0");
}

#[test]
fn manifest_string_escapes() {
    // Non-ASCII (here an escaped e-acute) becomes '?': the watch's font is 1-bit Latin.
    let j = format!(
        r#"{{"schema":1,"version":"1.0.0","url":"https://x/y.bin","size":5,"sha256":"{SHA}","notes":"say \"hi\"\\ \n café"}}"#
    );
    assert_eq!(parse_manifest(&j).unwrap().notes, "say \"hi\"\\ \n caf?");
}

#[test]
fn manifest_errors() {
    use ManifestError::*;
    let with = |body: &str| parse_manifest(&body.replace("%s", &format!(r#""sha256":"{SHA}""#)));

    // syntax
    assert_eq!(parse_manifest(""), Err(Malformed));
    assert_eq!(parse_manifest("[]"), Err(Malformed));
    assert_eq!(parse_manifest("<html>404</html>"), Err(Malformed));
    assert_eq!(parse_manifest(r#"{"schema":1,"version":"1.0.0""#), Err(Malformed));
    assert_eq!(parse_manifest(r#"{"schema":1,}x"#), Err(Malformed));
    assert_eq!(parse_manifest(r#"{"schema":1} trailing"#), Err(Malformed));
    // a truncated download must never parse as something valid
    for n in (1..GOOD_MANIFEST.len() - 1).step_by(7) {
        assert!(parse_manifest(&GOOD_MANIFEST[..n]).is_err(), "prefix of {n} bytes parsed");
    }

    // schema
    assert_eq!(with(r#"{"version":"1.0.0","url":"https://x/y","size":5,%s}"#), Err(MissingField));
    assert_eq!(with(r#"{"schema":2,"version":"1.0.0","url":"https://x/y","size":5,%s}"#), Err(UnsupportedSchema));
    // a future schema that changes field types is still "too new", not "missing"
    assert_eq!(parse_manifest(r#"{"schema":2,"version":7,"url":[]}"#), Err(UnsupportedSchema));

    // required fields
    assert_eq!(with(r#"{"schema":1,"url":"https://x/y","size":5,%s}"#), Err(MissingField)); // no version
    assert_eq!(with(r#"{"schema":1,"version":"1.0.0","size":5,%s}"#), Err(MissingField)); // no url
    assert_eq!(parse_manifest(r#"{"schema":1,"version":"1.0.0","url":"https://x/y","size":5}"#), Err(MissingField)); // no sha
    assert_eq!(with(r#"{"schema":1,"version":"1.0.0","url":"https://x/y",%s}"#), Err(MissingField)); // no size
    assert_eq!(with(r#"{"schema":1,"version":"1.0.0","url":"https://x/y","size":"5",%s}"#), Err(MissingField));
    assert_eq!(with(r#"{"schema":1,"version":"1.0.0","url":"https://x/y","size":0,%s}"#), Err(MissingField));
    assert_eq!(with(r#"{"schema":1,"version":"1.0.0","url":"https://x/y","size":-5,%s}"#), Err(MissingField));
    assert_eq!(with(r#"{"schema":1,"version":"1.0.0","url":"https://x/y","size":99999999999,%s}"#), Err(MissingField));
    assert_eq!(with(r#"{"schema":1,"version":"banana","url":"https://x/y","size":5,%s}"#), Err(MissingField));
    assert_eq!(
        with(r#"{"schema":1,"version":"1.0.0","min_version":"x","url":"https://x/y","size":5,%s}"#),
        Err(MissingField)
    );

    // url / hash
    assert_eq!(with(r#"{"schema":1,"version":"1.0.0","url":"http://x/y","size":5,%s}"#), Err(BadUrl));
    assert_eq!(with(r#"{"schema":1,"version":"1.0.0","url":"https://","size":5,%s}"#), Err(BadUrl));
    assert_eq!(with(r#"{"schema":1,"version":"1.0.0","url":"ftp://x/y","size":5,%s}"#), Err(BadUrl));
    assert_eq!(
        parse_manifest(r#"{"schema":1,"version":"1.0.0","url":"https://x/y","size":5,"sha256":"abc"}"#),
        Err(BadHash)
    );
    assert_eq!(
        parse_manifest(
            r#"{"schema":1,"version":"1.0.0","url":"https://x/y","size":5,"sha256":"zb4d3ec930a1330ac9585a5667a34810be7f6885d979a301d13b8d3da0557c9b"}"#
        ),
        Err(BadHash)
    );
}

// ---- decision -------------------------------------------------------------

fn manifest(version: &str, min_version: &str) -> Manifest {
    Manifest {
        schema: 1,
        version: version.into(),
        min_version: min_version.into(),
        url: String::new(),
        sha256: String::new(),
        notes: String::new(),
        size: 0,
    }
}

#[test]
fn decide_release_builds() {
    use Decision::*;
    let m = |v| manifest(v, "0.0.0");
    assert_eq!(decide("0.1.0", &m("0.2.0")), Available);
    assert_eq!(decide("0.2.0", &m("0.2.0")), UpToDate);
    assert_eq!(decide("0.3.0", &m("0.2.0")), UpToDate); // never downgrade
    assert_eq!(decide("0.1.0-beta.1", &m("0.1.0")), Available);
    assert_eq!(decide("0.1.0-beta.1", &m("0.1.0-beta.2")), Available);
    assert_eq!(decide("0.1.0", &m("0.1.0-beta.2")), UpToDate);
}

#[test]
fn decide_min_version_gate() {
    use Decision::*;
    // can't be applied over the air: ask for a USB install instead
    assert_eq!(decide("0.1.0", &manifest("0.3.0", "0.2.0")), NeedsUsbInstall);
    assert_eq!(decide("0.2.0", &manifest("0.3.0", "0.2.0")), Available);
    assert_eq!(decide("0.2.5", &manifest("0.3.0", "0.2.0")), Available);
}

#[test]
fn decide_development_builds() {
    use Decision::*;
    let m = |v| manifest(v, "0.0.0");
    // not a comparable version: offer whatever is published
    assert_eq!(decide("b3793a2", &m("0.1.0")), Available);
    assert_eq!(decide("dev", &m("0.1.0")), Available);
    assert_eq!(decide("unknown", &m("0.1.0")), Available);
    // ...without applying the USB-only gate to a developer's own build
    assert_eq!(decide("dev", &manifest("0.3.0", "0.2.0")), Available);
    // a git-describe string does parse (as a prerelease of the last tag), so it sorts below that
    // release and is offered it
    assert_eq!(decide("0.1.0-3-gabc1234", &m("0.1.0")), Available);
    assert_eq!(decide("0.1.0-3-gabc1234-dirty", &m("0.1.0")), Available);
}

#[test]
fn decide_never_offers_garbage() {
    assert_eq!(decide("0.1.0", &manifest("banana", "0.0.0")), Decision::UpToDate);
}

// ---- helpers --------------------------------------------------------------

#[test]
fn battery_gate() {
    assert!(!battery_allows_update(10, false));
    assert!(!battery_allows_update(39, false));
    assert!(battery_allows_update(40, false));
    assert!(battery_allows_update(100, false));
    assert!(battery_allows_update(5, true)); // on a charger: always fine
    assert!(battery_allows_update(-1, false)); // no battery reading: must be on USB
}

#[test]
fn html_escape_cases() {
    assert_eq!(html_escape("plain"), "plain");
    assert_eq!(
        html_escape("<script>alert(\"x\")</script>"),
        "&lt;script&gt;alert(&quot;x&quot;)&lt;/script&gt;"
    );
    assert_eq!(html_escape("Bob's & Alice's"), "Bob&#39;s &amp; Alice&#39;s");
    assert_eq!(html_escape(""), "");
}

#[test]
fn make_password_cases() {
    // deterministic mapping: index 0 -> 'A', 22 -> 'Z', 23 -> '2', 30 -> '9'
    assert_eq!(make_password(&[0; 16]), "AAAAAAAAAA");
    assert_eq!(make_password(&[22, 23, 30, 31, 0]), "Z29AA"); // 31 wraps to index 0

    // bytes in the biased tail (>= 248) are discarded, not folded in
    assert_eq!(make_password(&[255, 250, 248, 0, 1]), "AB");

    // never longer than PASSWORD_LEN, and only unambiguous characters
    let all: Vec<u8> = (0..=255).collect();
    let pw = make_password(&all);
    assert_eq!(pw.len(), PASSWORD_LEN);
    for c in pw.chars() {
        assert!("ABCDEFGHJKMNPQRSTUVWXYZ23456789".contains(c));
        assert!(!"ILO01".contains(c));
    }
    assert_eq!(make_password(&[]), "");
}
