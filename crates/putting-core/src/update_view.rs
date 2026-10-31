//! What the update-mode screens show: the updater's status, and the pure mapping from that status
//! to the text, buttons and graphics of each screen. Ported from the C++ updater (wifi_ota.h's
//! Phase/Status and update_ui.cpp's viewFor). The firmware draws a `View`; nothing here knows
//! about fonts or pixels beyond the word wrap taking a measuring function.

use crate::update::MIN_BATTERY_PCT;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Phase {
    #[default]
    Starting,
    /// Looking for networks before opening the setup access point.
    Scanning,
    /// Access point and portal up, waiting for the phone.
    Provisioning,
    /// Joining a Wi-Fi network.
    Connecting,
    /// Clock and manifest.
    Checking,
    UpToDate,
    /// A newer version can be installed.
    Available,
    /// Newer, but the running firmware is too old to update over the air.
    NeedsUsbInstall,
    Downloading,
    /// Written; the watch is about to restart into it.
    Installed,
    Error,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Status {
    pub phase: Phase,
    /// Downloading: 0..=100.
    pub progress_pct: u8,
    /// Provisioning: a phone has joined the watch's setup network.
    pub portal_client: bool,
    /// Provisioning: the setup network's name, one-time password, and the page's address.
    pub ap_ssid: String,
    pub ap_pass: String,
    pub ap_ip: String,
    /// Connecting: the network being joined.
    pub ssid: String,
    /// UpToDate/Available/NeedsUsbInstall: the published version and its one-line summary.
    pub version: String,
    pub notes: String,
    /// Error: what went wrong. Provisioning: why the previous join failed, if it did.
    pub error: String,
}

/// Everything a phase wants on screen. Empty strings mean "not shown".
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct View {
    pub title: String,
    pub body: String,
    pub note: String,
    pub primary: String,
    pub secondary: String,
    pub link: String,
    pub qr_data: String,
    pub title_small: bool,
    pub busy: bool,
    pub qr: bool,
    pub bar: bool,
    pub pct: bool,
    pub primary_enabled: bool,
    /// The note is a warning (drawn in a warm colour).
    pub warn: bool,
    pub progress: u8,
}

fn site_without_scheme(site: &str) -> &str {
    site.split_once("://").map_or(site, |(_, rest)| rest)
}

/// The screen for `st`. `running` is this firmware's version, `site` the update site's URL.
pub fn view_for(st: &Status, running: &str, battery_ok: bool, site: &str) -> View {
    let mut v = View { primary_enabled: true, ..Default::default() };
    let s = |x: &str| x.to_string();
    match st.phase {
        Phase::Starting => {
            v.title = s("Starting...");
            v.busy = true;
            v.secondary = s("Cancel");
        }
        Phase::Scanning => {
            v.title = s("Looking for\nWi-Fi networks");
            v.busy = true;
            v.secondary = s("Cancel");
        }
        Phase::Provisioning => {
            v.title = s("Set up Wi-Fi");
            v.title_small = true;
            v.qr = true;
            if st.portal_client {
                // Joined the watch's network. The sign-in sheet usually opens the page by itself;
                // if it didn't, this opens it.
                v.qr_data = format!("http://{}/", st.ap_ip);
                v.body = s("If the page didn't open,\nscan this to open it");
            } else {
                v.qr_data = format!("WIFI:T:WPA;S:{};P:{};;", st.ap_ssid, st.ap_pass);
                // No manual line break in the instruction: it wraps by itself into two balanced lines.
                v.body = if st.error.is_empty() {
                    s("Scan with your phone's camera, then tap Join")
                } else {
                    format!("{}\nScan to try again.", st.error)
                };
            }
            v.note = format!("Or join {}\nPassword: {}", st.ap_ssid, st.ap_pass);
            v.secondary = s("Cancel");
        }
        Phase::Connecting => {
            v.title = s("Joining Wi-Fi");
            v.body = st.ssid.clone();
            v.busy = true;
            v.secondary = s("Cancel");
        }
        Phase::Checking => {
            v.title = s("Checking for\nupdates");
            v.busy = true;
            v.secondary = s("Cancel");
        }
        Phase::UpToDate => {
            v.title = s("You're up to date");
            v.body = format!("Version {running}");
            v.primary = s("Done");
            v.link = s("Change Wi-Fi");
        }
        Phase::Available => {
            v.title = s("Update available");
            v.body = format!("Version {}\n(you have {running})", st.version);
            if !st.notes.is_empty() {
                v.body += &format!("\n{}", st.notes);
            }
            if !battery_ok {
                v.note = format!("Battery is low. Plug in a charger,\nor charge to {MIN_BATTERY_PCT}%, to install.");
                v.warn = true;
            }
            v.primary = s("Install");
            v.primary_enabled = battery_ok;
            v.secondary = s("Not now");
            v.link = s("Change Wi-Fi");
        }
        Phase::NeedsUsbInstall => {
            v.title = s("USB install\nneeded");
            // Host and path on separate lines: together they are wider than the screen and would
            // break in the middle of the host name.
            let site = site_without_scheme(site);
            let (host, path) = site.split_at(site.find('/').unwrap_or(site.len()));
            v.body = format!("Version {} can't be installed from the watch.\nOn a computer, open\n{host}", st.version);
            if !path.is_empty() {
                v.body += &format!("\n{path}");
            }
            v.secondary = s("Close");
        }
        Phase::Downloading => {
            v.title = s("Downloading\nupdate");
            v.bar = true;
            v.pct = true;
            v.progress = st.progress_pct;
            v.note = s("Keep the watch on until it restarts");
            v.secondary = s("Cancel");
        }
        Phase::Installed => {
            v.title = s("Update installed");
            v.body = s("Restarting...");
        }
        Phase::Error => {
            v.title = s("Something\nwent wrong");
            v.body = st.error.clone();
            v.primary = s("Try again");
            v.secondary = s("Cancel");
            v.link = s("Change Wi-Fi");
        }
    }
    v
}

/// Greedy word wrap to `max_width` pixels, given a function that measures a string's width. Explicit
/// newlines are kept; a single word wider than the limit gets a line to itself.
pub fn wrap_by(text: &str, max_width: i32, measure: impl Fn(&str) -> i32) -> String {
    let mut out = Vec::new();
    for para in text.split('\n') {
        let mut line = String::new();
        for word in para.split_whitespace() {
            if line.is_empty() {
                line = word.to_string();
                continue;
            }
            let candidate = format!("{line} {word}");
            if measure(&candidate) <= max_width {
                line = candidate;
            } else {
                out.push(core::mem::take(&mut line));
                line = word.to_string();
            }
        }
        out.push(line);
    }
    out.join("\n")
}
