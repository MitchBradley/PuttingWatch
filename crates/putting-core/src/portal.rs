//! The Wi-Fi setup portal's pure parts, ported from the C++ updater (wifi_ota.cpp): the pages, form
//! parsing and validation, the list of scanned networks, and the wildcard DNS reply that makes a
//! phone's captive-portal sheet open. No ESP dependencies, so it is tested on the host.

use crate::update::html_escape;

/// A scanned network as offered on the setup page.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Network {
    pub ssid: String,
    /// No password needed.
    pub open: bool,
    pub rssi: i32,
}

/// At most this many distinct networks are listed.
pub const MAX_NETWORKS: usize = 24;

/// Clean up raw scan results `(ssid, rssi, open)`: drop empty or over-long names, keep one entry
/// per name (the strongest, whose security decides `open`), strongest first, at most
/// [`MAX_NETWORKS`] names.
pub fn collect_networks(raw: impl IntoIterator<Item = (String, i32, bool)>) -> Vec<Network> {
    let mut nets: Vec<Network> = Vec::new();
    for (ssid, rssi, open) in raw {
        if ssid.is_empty() || ssid.len() > 32 {
            continue;
        }
        if let Some(i) = nets.iter().position(|n| n.ssid == ssid) {
            if rssi > nets[i].rssi {
                nets[i].rssi = rssi;
                nets[i].open = open;
            }
        } else if nets.len() < MAX_NETWORKS {
            nets.push(Network { ssid, open, rssi });
        }
    }
    nets.sort_by(|a, b| b.rssi.cmp(&a.rssi));
    nets
}

// --- the pages ------------------------------------------------------------------------------------

const PAGE_HEAD: &str = concat!(
    "<!doctype html><html lang=en><head><meta charset=utf-8>",
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">",
    "<title>Honu Watch Wi-Fi</title><style>",
    ":root{color-scheme:light dark}",
    "body{font:18px/1.45 system-ui,sans-serif;max-width:30rem;margin:0 auto;padding:20px}",
    "h1{font-size:1.45rem;margin:.2rem 0 .6rem}",
    "label{display:block;margin:1.1rem 0 .3rem;font-weight:600}",
    "select,input[type=text],input[type=password],button{width:100%;box-sizing:border-box;",
    "font:inherit;padding:.75rem;border-radius:.6rem;border:1px solid #8888}",
    "button{background:#1b7f3b;color:#fff;border:0;margin-top:1.6rem;font-weight:600}",
    ".note{color:#888;font-size:.9rem}",
    ".show{display:flex;gap:.5rem;align-items:center;font-weight:400;margin-top:.6rem}",
    ".show input{width:auto}.err{color:#c0392b;font-weight:600}",
    "</style></head><body>"
);

/// The form: pick a network (or type a hidden one's name) and enter its password.
pub fn portal_page(nets: &[Network]) -> String {
    let mut h = String::from(PAGE_HEAD);
    h += "<h1>Connect your watch to Wi-Fi</h1>\
         <p class=note>The watch needs your Wi-Fi to download updates. \
         It can only use 2.4&nbsp;GHz networks.</p>\
         <form method=post action=/connect>\
         <label for=n>Wi-Fi network</label>\
         <select id=n name=ssid onchange=\"document.getElementById('o').style.display=\
         this.value=='__other'?'block':'none'\">";
    for n in nets {
        let e = html_escape(&n.ssid);
        h += &format!("<option value=\"{e}\">{e}{}</option>", if n.open { " (open)" } else { "" });
    }
    h += "<option value=\"__other\">Other / hidden network...</option></select>";
    h += "<input type=text id=o name=other placeholder=\"Network name\" autocapitalize=off \
         autocorrect=off style=\"margin-top:.5rem;display:";
    h += if nets.is_empty() { "block" } else { "none" };
    h += "\"><label for=p>Password</label>\
         <input type=password id=p name=pass autocomplete=off autocapitalize=off>\
         <label class=show><input type=checkbox onclick=\"document.getElementById('p').type=\
         this.checked?'text':'password'\"> Show password</label>\
         <button type=submit>Connect</button></form></body></html>";
    h
}

/// A one-message page (the reply after submitting, or a validation error with a way back).
pub fn message_page(title: &str, body_html: &str, is_error: bool) -> String {
    let mut h = String::from(PAGE_HEAD);
    h += &format!("<h1>{title}</h1><p{}>{body_html}</p>", if is_error { " class=err" } else { "" });
    if is_error {
        h += "<p><a href=\"/\">Go back</a></p>";
    }
    h += "</body></html>";
    h
}

/// The reply once credentials are accepted.
pub fn joining_page(ssid: &str) -> String {
    message_page(
        "Check your watch",
        &format!(
            "The watch is now joining <b>{}</b>. You can close this page; the result shows on the watch.",
            html_escape(ssid)
        ),
        false,
    )
}

// --- the form -------------------------------------------------------------------------------------

/// Decode an application/x-www-form-urlencoded value: '+' is a space, %XX a byte. A '%' not
/// followed by two hex digits is kept literally.
pub fn url_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'+' => out.push(b' '),
            b'%' if i + 2 < b.len() && b[i + 1].is_ascii_hexdigit() && b[i + 2].is_ascii_hexdigit() => {
                out.push(u8::from_str_radix(&s[i + 1..i + 3], 16).unwrap());
                i += 2;
            }
            c => out.push(c),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Split a urlencoded form body into decoded (name, value) pairs.
pub fn parse_form(body: &str) -> Vec<(String, String)> {
    body.split('&')
        .filter(|kv| !kv.is_empty())
        .map(|kv| {
            let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
            (url_decode(k), url_decode(v))
        })
        .collect()
}

fn field<'a>(form: &'a [(String, String)], name: &str) -> &'a str {
    form.iter().find(|(k, _)| k == name).map_or("", |(_, v)| v.as_str())
}

/// Check what the phone submitted. Returns (ssid, password), or the message to show the user.
pub fn validate_connect(form: &[(String, String)]) -> Result<(String, String), &'static str> {
    let mut ssid = field(form, "ssid");
    if ssid == "__other" {
        ssid = field(form, "other");
    }
    let ssid = ssid.trim();
    let pass = field(form, "pass");
    if ssid.is_empty() || ssid.len() > 32 {
        return Err("Please choose a network, or type its name.");
    }
    if !pass.is_empty() && !(8..=63).contains(&pass.len()) {
        return Err("A Wi-Fi password is between 8 and 63 characters.");
    }
    Ok((ssid.to_string(), pass.to_string()))
}

// --- wildcard DNS -----------------------------------------------------------------------------------

/// Answer a DNS query with `ip` for every name, so any lookup a phone makes lands on the watch
/// (which is what makes the phone's sign-in sheet open). A queries get one A record; anything else
/// (AAAA, ...) gets an empty NOERROR answer so the phone falls back to IPv4. None for packets that
/// aren't a single-question standard query.
pub fn dns_reply(query: &[u8], ip: [u8; 4]) -> Option<Vec<u8>> {
    if query.len() < 12 {
        return None;
    }
    let flags = u16::from_be_bytes([query[2], query[3]]);
    let qdcount = u16::from_be_bytes([query[4], query[5]]);
    // QR must be 0 (a query), opcode 0 (standard), exactly one question.
    if flags & 0x8000 != 0 || (flags >> 11) & 0xF != 0 || qdcount != 1 {
        return None;
    }
    // Walk the question's name to find where it ends.
    let mut p = 12;
    loop {
        let len = *query.get(p)? as usize;
        if len == 0 {
            p += 1;
            break;
        }
        if len & 0xC0 != 0 {
            return None; // compression pointers don't appear in a query's question
        }
        p += 1 + len;
    }
    let qend = p + 4; // type + class
    if qend > query.len() {
        return None;
    }
    let (qtype, qclass) = (u16::from_be_bytes([query[p], query[p + 1]]), u16::from_be_bytes([query[p + 2], query[p + 3]]));
    let answer = qtype == 1 && qclass == 1;

    let mut r = Vec::with_capacity(qend + 16);
    r.extend_from_slice(&query[0..2]); // id
    let rd = flags & 0x0100;
    r.extend_from_slice(&(0x8000 | rd | 0x0080u16).to_be_bytes()); // response, RD echoed, RA, NOERROR
    r.extend_from_slice(&1u16.to_be_bytes()); // qdcount
    r.extend_from_slice(&(answer as u16).to_be_bytes()); // ancount
    r.extend_from_slice(&[0, 0, 0, 0]); // nscount, arcount
    r.extend_from_slice(&query[12..qend]); // the question
    if answer {
        r.extend_from_slice(&[0xC0, 0x0C]); // name: pointer to the question
        r.extend_from_slice(&1u16.to_be_bytes()); // A
        r.extend_from_slice(&1u16.to_be_bytes()); // IN
        r.extend_from_slice(&60u32.to_be_bytes()); // ttl
        r.extend_from_slice(&4u16.to_be_bytes());
        r.extend_from_slice(&ip);
    }
    Some(r)
}
