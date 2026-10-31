use std::process::Command;

/// The firmware's version, from git, the same way the C++ build gets it (tools/fw_version.py in
/// PuttingWatch): `git describe --tags --always --dirty`, with a leading "v" dropped. A build on a
/// release tag `v1.2.0` reports exactly "1.2.0" - the string the OTA manifest carries. Between tags it
/// is "1.2.0-3-gabc1234" (and "-dirty" with uncommitted changes), which the updater treats as a
/// development build. Without git it is "unknown".
fn firmware_version() -> String {
    let described = Command::new("git")
        .args(["describe", "--tags", "--always", "--dirty"])
        .output()
        .ok()
        .filter(|o| o.status.success())
        .and_then(|o| String::from_utf8(o.stdout).ok())
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| "unknown".to_string());
    match described.strip_prefix('v') {
        Some(rest) if rest.starts_with(|c: char| c.is_ascii_digit()) => rest.to_string(),
        _ => described,
    }
}

fn main() {
    embuild::espidf::sysenv::output();

    println!("cargo:rustc-env=FW_VERSION={}", firmware_version());
    // Re-evaluate when a commit or tag is made, and when files change (for the "-dirty" suffix). The
    // value only reaches the compiler, and only recompiles anything, when the string itself changes.
    for path in [".git/HEAD", ".git/refs", ".git/packed-refs", "src", "crates", "assets", "Cargo.toml"] {
        println!("cargo:rerun-if-changed={path}");
    }
}
