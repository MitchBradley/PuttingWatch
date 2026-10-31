use putting_core::portal::*;

fn form(body: &str) -> Vec<(String, String)> {
    parse_form(body)
}

#[test]
fn url_decoding() {
    assert_eq!(url_decode("plain"), "plain");
    assert_eq!(url_decode("a+b"), "a b");
    assert_eq!(url_decode("a%20b"), "a b");
    assert_eq!(url_decode("caf%C3%A9"), "café");
    assert_eq!(url_decode("100%25"), "100%");
    // a '%' that isn't followed by two hex digits is kept as typed
    assert_eq!(url_decode("50%"), "50%");
    assert_eq!(url_decode("5%z1"), "5%z1");
    assert_eq!(url_decode("%4"), "%4");
    assert_eq!(url_decode(""), "");
    // invalid UTF-8 doesn't panic
    assert!(!url_decode("%FF%FE").is_empty());
}

#[test]
fn form_parsing() {
    assert_eq!(
        form("ssid=My+Net&other=&pass=p%40ss%26word"),
        vec![("ssid".into(), "My Net".into()), ("other".into(), "".into()), ("pass".into(), "p@ss&word".into())]
    );
    assert_eq!(form("novalue"), vec![("novalue".into(), "".into())]);
    assert!(form("").is_empty());
    assert_eq!(form("a=1&&b=2").len(), 2);
}

#[test]
fn connect_validation() {
    assert_eq!(validate_connect(&form("ssid=Home&pass=12345678")), Ok(("Home".into(), "12345678".into())));
    // an open network has no password
    assert_eq!(validate_connect(&form("ssid=Cafe&pass=")), Ok(("Cafe".into(), "".into())));
    // the "other / hidden" choice takes its name from the other field, trimmed
    assert_eq!(
        validate_connect(&form("ssid=__other&other=++Hidden+Net++&pass=12345678")),
        Ok(("Hidden Net".into(), "12345678".into()))
    );
    assert!(validate_connect(&form("ssid=&pass=12345678")).is_err());
    assert!(validate_connect(&form("ssid=__other&other=&pass=12345678")).is_err());
    assert!(validate_connect(&form("pass=12345678")).is_err());
    let long = "x".repeat(33);
    assert!(validate_connect(&form(&format!("ssid={long}&pass=12345678"))).is_err());
    assert!(validate_connect(&form(&format!("ssid={}&pass=12345678", "x".repeat(32)))).is_ok());
    // passwords: 8..=63 or empty
    assert!(validate_connect(&form("ssid=Home&pass=1234567")).is_err());
    assert!(validate_connect(&form(&format!("ssid=Home&pass={}", "p".repeat(64)))).is_err());
    assert!(validate_connect(&form(&format!("ssid=Home&pass={}", "p".repeat(63)))).is_ok());
}

#[test]
fn scan_list_is_cleaned_up() {
    let raw = vec![
        ("Home".to_string(), -70, false),
        ("".to_string(), -30, false),                // hidden network: no name
        ("x".repeat(33), -30, false),                // too long to be a real SSID
        ("Cafe".to_string(), -80, true),
        ("Home".to_string(), -50, true),             // same name, stronger: its security wins
        ("Home".to_string(), -90, false),            // weaker duplicate: ignored
        ("Far".to_string(), -95, false),
    ];
    let nets = collect_networks(raw);
    let names: Vec<_> = nets.iter().map(|n| n.ssid.as_str()).collect();
    assert_eq!(names, ["Home", "Cafe", "Far"]); // strongest first
    assert_eq!(nets[0], Network { ssid: "Home".into(), open: true, rssi: -50 });

    // never more than MAX_NETWORKS distinct names
    let many = (0..40).map(|i| (format!("net{i}"), -40 - i, false));
    assert_eq!(collect_networks(many).len(), MAX_NETWORKS);
}

#[test]
fn pages_escape_network_names() {
    let evil = Network { ssid: "<script>alert(1)</script>\"".into(), open: false, rssi: -40 };
    let page = portal_page(&[evil]);
    assert!(!page.contains("<script>alert"));
    assert!(page.contains("&lt;script&gt;"));
    assert!(page.contains("name=ssid"));
    assert!(page.contains("action=/connect"));
    // the hidden-network box starts visible only when nothing was found
    assert!(portal_page(&[]).contains("margin-top:.5rem;display:block"));
    assert!(portal_page(&[Network { ssid: "a".into(), open: true, rssi: 0 }]).contains("margin-top:.5rem;display:none"));
    assert!(portal_page(&[Network { ssid: "a".into(), open: true, rssi: 0 }]).contains("a (open)"));

    assert!(joining_page("<b>x</b>").contains("&lt;b&gt;x&lt;/b&gt;"));
    assert!(message_page("Oops", "bad", true).contains("Go back"));
    assert!(!message_page("Ok", "fine", false).contains("Go back"));
}

/// A standard A query for `name`, id 0x1234, recursion desired.
fn query(name: &str, qtype: u16) -> Vec<u8> {
    let mut q = vec![0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0];
    for label in name.split('.') {
        q.push(label.len() as u8);
        q.extend_from_slice(label.as_bytes());
    }
    q.push(0);
    q.extend_from_slice(&qtype.to_be_bytes());
    q.extend_from_slice(&1u16.to_be_bytes());
    q
}

#[test]
fn dns_answers_a_queries_with_the_watch() {
    let q = query("connectivitycheck.gstatic.com", 1);
    let r = dns_reply(&q, [192, 168, 4, 1]).unwrap();
    assert_eq!(&r[0..2], &[0x12, 0x34]); // same id
    assert_eq!(r[2] & 0x80, 0x80); // a response
    assert_eq!(r[2] & 0x01, 0x01); // recursion desired echoed
    assert_eq!(r[3] & 0x8F, 0x80); // RA set, rcode NOERROR
    assert_eq!(&r[4..8], &[0, 1, 0, 1]); // one question, one answer
    assert_eq!(&r[12..q.len()], &q[12..]); // the question is echoed
    assert_eq!(&r[r.len() - 4..], &[192, 168, 4, 1]);
    assert_eq!(&r[q.len()..q.len() + 2], &[0xC0, 0x0C]); // answer name points at the question
}

#[test]
fn dns_gives_non_a_queries_an_empty_answer() {
    let q = query("example.com", 28); // AAAA
    let r = dns_reply(&q, [192, 168, 4, 1]).unwrap();
    assert_eq!(&r[6..8], &[0, 0]); // no answers
    assert_eq!(r.len(), q.len()); // header + question only
}

#[test]
fn dns_ignores_junk() {
    assert!(dns_reply(&[], [1, 2, 3, 4]).is_none());
    assert!(dns_reply(&[0; 11], [1, 2, 3, 4]).is_none());
    let mut response = query("a.b", 1);
    response[2] |= 0x80; // already a response
    assert!(dns_reply(&response, [1, 2, 3, 4]).is_none());
    let mut two = query("a.b", 1);
    two[5] = 2; // two questions
    assert!(dns_reply(&two, [1, 2, 3, 4]).is_none());
    let mut truncated = query("a.b", 1);
    truncated.truncate(truncated.len() - 3);
    assert!(dns_reply(&truncated, [1, 2, 3, 4]).is_none());
    let mut opcode = query("a.b", 1);
    opcode[2] |= 0x10; // not a standard query
    assert!(dns_reply(&opcode, [1, 2, 3, 4]).is_none());
}
