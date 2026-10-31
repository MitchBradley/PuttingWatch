use putting_core::update_view::*;

const SITE: &str = "https://mitchbradley.github.io/PuttingWatch/";

fn st(phase: Phase) -> Status {
    Status { phase, ..Default::default() }
}

#[test]
fn busy_phases_offer_cancel() {
    for p in [Phase::Starting, Phase::Scanning, Phase::Connecting, Phase::Checking] {
        let v = view_for(&st(p), "0.1.0", true, SITE);
        assert!(v.busy, "{p:?}");
        assert_eq!(v.secondary, "Cancel");
        assert!(v.primary.is_empty());
    }
    assert_eq!(view_for(&Status { phase: Phase::Connecting, ssid: "Home".into(), ..Default::default() }, "0.1.0", true, SITE).body, "Home");
}

#[test]
fn provisioning_shows_a_join_qr_then_a_page_qr() {
    let base = Status {
        phase: Phase::Provisioning,
        ap_ssid: "HonuWatch-1A2B".into(),
        ap_pass: "ABCDEFGH23".into(),
        ap_ip: "192.168.71.1".into(),
        ..Default::default()
    };
    let v = view_for(&base, "0.1.0", true, SITE);
    assert!(v.qr && v.title_small);
    assert_eq!(v.qr_data, "WIFI:T:WPA;S:HonuWatch-1A2B;P:ABCDEFGH23;;");
    assert!(v.body.contains("Scan with your phone's camera"));
    assert_eq!(v.note, "Or join HonuWatch-1A2B\nPassword: ABCDEFGH23");

    // a phone has joined: the code now opens the page
    let v = view_for(&Status { portal_client: true, ..base.clone() }, "0.1.0", true, SITE);
    assert_eq!(v.qr_data, "http://192.168.71.1/");
    assert!(v.body.contains("didn't open"));

    // after a failed attempt the reason leads the instruction
    let v = view_for(&Status { error: "Couldn't find \"Home\".".into(), ..base }, "0.1.0", true, SITE);
    assert_eq!(v.body, "Couldn't find \"Home\".\nScan to try again.");
}

#[test]
fn available_gates_install_on_the_battery() {
    let s = Status { phase: Phase::Available, version: "0.2.0".into(), notes: "Faster".into(), ..Default::default() };
    let ok = view_for(&s, "0.1.0", true, SITE);
    assert_eq!(ok.title, "Update available");
    assert_eq!(ok.body, "Version 0.2.0\n(you have 0.1.0)\nFaster");
    assert!(ok.primary_enabled && !ok.warn && ok.note.is_empty());
    assert_eq!((ok.primary.as_str(), ok.secondary.as_str(), ok.link.as_str()), ("Install", "Not now", "Change Wi-Fi"));

    let low = view_for(&s, "0.1.0", false, SITE);
    assert!(!low.primary_enabled && low.warn);
    assert!(low.note.contains("charge to 40%"));

    // no notes: no empty line
    let bare = view_for(&Status { notes: String::new(), ..s }, "0.1.0", true, SITE);
    assert_eq!(bare.body, "Version 0.2.0\n(you have 0.1.0)");
}

#[test]
fn usb_install_splits_host_and_path() {
    let s = Status { phase: Phase::NeedsUsbInstall, version: "0.3.0".into(), ..Default::default() };
    let v = view_for(&s, "0.1.0", true, SITE);
    assert_eq!(
        v.body,
        "Version 0.3.0 can't be installed from the watch.\nOn a computer, open\nmitchbradley.github.io\n/PuttingWatch/"
    );
    assert_eq!(v.secondary, "Close");
    // a site with no path has no path line
    let v = view_for(&s, "0.1.0", true, "https://example.com");
    assert!(v.body.ends_with("open\nexample.com"));
}

#[test]
fn downloading_shows_progress() {
    let v = view_for(&Status { phase: Phase::Downloading, progress_pct: 42, ..Default::default() }, "0.1.0", true, SITE);
    assert!(v.bar && v.pct);
    assert_eq!(v.progress, 42);
    assert_eq!(v.secondary, "Cancel");
}

#[test]
fn results() {
    let up = view_for(&st(Phase::UpToDate), "0.1.5", true, SITE);
    assert_eq!((up.title.as_str(), up.body.as_str(), up.primary.as_str()), ("You're up to date", "Version 0.1.5", "Done"));
    let done = view_for(&st(Phase::Installed), "0.1.0", true, SITE);
    assert_eq!(done.title, "Update installed");
    assert!(done.primary.is_empty() && done.secondary.is_empty());
    let err = view_for(&Status { phase: Phase::Error, error: "Oops".into(), ..Default::default() }, "0.1.0", true, SITE);
    assert_eq!((err.body.as_str(), err.primary.as_str(), err.secondary.as_str(), err.link.as_str()), ("Oops", "Try again", "Cancel", "Change Wi-Fi"));
}

#[test]
fn wrapping_by_measured_width() {
    let width = |s: &str| s.len() as i32 * 10; // every char is 10 px
    assert_eq!(wrap_by("one two three four", 100, width), "one two\nthree four");
    assert_eq!(wrap_by("short", 100, width), "short");
    // explicit newlines survive
    assert_eq!(wrap_by("ab\ncd ef", 100, width), "ab\ncd ef");
    // a word wider than the limit gets its own line instead of looping
    assert_eq!(wrap_by("a verylongwordhere b", 50, width), "a\nverylongwordhere\nb");
    assert_eq!(wrap_by("", 100, width), "");
    // a blank line in the middle is kept
    assert_eq!(wrap_by("a\n\nb", 100, width), "a\n\nb");
}
