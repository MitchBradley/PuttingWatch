# Honu Watch (Rust)

A Rust port of the Honu Watch app — a putting-stroke trainer for the
[Waveshare ESP32-C6-Touch-AMOLED-2.06](https://www.waveshare.com/) — written against
`esp-idf-svc` (std) as a test of how mature the Rust ESP32 ecosystem is. The C++ firmware
([PuttingWatch](https://github.com/MitchBradley/PuttingWatch)) is the reference; this port has the same
features and is **interoperable with it over the air**: a watch can be updated from C++ to Rust, from Rust
to C++ and from Rust to Rust, and a bad image rolls itself back.

## What it does

Strap the watch to a putter. It calibrates its rest position, then clicks (a chirp) each time the backswing
crosses an angle threshold, speaks the peak of each stroke as a sensitivity level, and says "ready" when it is
armed for the next one. The − / + buttons set the click interval (levels 1–16); Recal restarts calibration.

Also ported: battery readout, run-screen blanking, light sleep (double-tap), PMU power-off (Stop / BOOT), and
**update mode** — Wi-Fi setup through the watch's own network (QR codes), checking the update site and
installing a new image over HTTPS.

## Repository layout

| Path | What |
|---|---|
| `src/main.rs` | Boot, the 100 Hz sampling loop, touch, power state machine |
| `src/co5300.rs`, `gfx.rs`, `ui.rs` | AMOLED over QSPI (`esp_lcd`), `embedded-graphics` strip renderer, the app's screens |
| `src/touch.rs`, `pmu.rs` | FT3168 touch, AXP2101 (battery, shutdown) |
| `src/audio.rs`, `voice_index.rs` | ES8311 codec + I2S, synthesized click, spoken cues and numbers |
| `src/update_mode.rs`, `update_ui.rs`, `provision.rs` | Update mode: engine, screens, Wi-Fi setup portal |
| `crates/putting-core/` | **Pure, host-testable logic**: swing tracker, gravity filter, the updater's version / manifest / decision logic, the portal's pages and DNS reply, the update screens' text |
| `assets/` | Spoken samples (`voice/*.wav`, `voice.bin`) and the two Honu images (`*.rgb565`) |
| `tools/` | Generators for those assets (stdlib Python) |
| `partitions.csv` | The 16 MB dual-OTA table — **identical to the C++ firmware's** |
| `sdkconfig.defaults` | IDF settings that must match the C++ bootloader (see below) |

## Setting up

You need Rust via [rustup](https://rustup.rs), plus:

```sh
cargo install espflash ldproxy
```

`rust-toolchain.toml` pins a **nightly** (the `riscv32imac-esp-espidf` target is tier 3, so std is built from
source with `build-std`). The first build clones and builds ESP-IDF v5.5.3 into `.embuild/` — **about 5 GB**
(the IDF tree, the RISC-V toolchain and a Python environment) and several minutes — and the build output in
`target/` is another 3 GB or so. Keep the checkout somewhere iCloud doesn't sync (not `~/Documents`): evicted
files make builds and git fail in confusing ways (`mmap failed: Operation canceled`).

Two environment gotchas:

- **Deactivate any Python virtualenv** (or `env -u VIRTUAL_ENV`) — the IDF installer refuses to run inside one.
- The nightly is pinned to a date because current nightlies don't build std for ESP-IDF against `libc 0.2.191`.
  Move the pin forward only after checking that `cargo build` still works.

## Build

```sh
cargo build            # debug (opt-level z, symbols), what you flash while developing
cargo build --release  # what is published
```

`Cargo.lock` is committed on purpose: the `[patch.crates-io]` entries track an unpinned `esp-idf` git branch, so
the lockfile is what makes a build reproducible.

### Wi-Fi credentials for the first update

The watch normally gets its Wi-Fi through the setup portal (Bye → Update → Change Wi-Fi). To skip that on a
bench, bake credentials in **at build time, from your own shell** — they are saved to NVS on first use and are
never in the source tree:

```sh
WIFI_SSID='network' WIFI_PASS='password' cargo build
```

The resulting ELF contains the password. Don't publish it, and build releases with those variables unset.

## Flash

Flash with the project's partition table **and the IDF-built bootloader**:

```sh
espflash flash --port /dev/cu.usbmodemXXXX \
  --partition-table partitions.csv \
  --bootloader target/riscv32imac-esp-espidf/debug/build/esp-idf-sys/*/out/build/bootloader/bootloader.bin \
  --flash-size 16mb \
  target/riscv32imac-esp-espidf/debug/putting-watch-rs
```

- The `*` in the bootloader path is the esp-idf-sys build hash; if several builds have left more than one match,
  use the newest (`ls -t ... | head -1`).
- Without `--bootloader`/`--partition-table`, `espflash` writes its own bundled bootloader (a different IDF
  version) and a single-app table, and the OTA machinery doesn't work.
- **Don't use `flash --monitor` just to look at the log** — it reflashes every time, with whatever flags you gave.
  To watch the console: `espflash monitor --port ...`.
- Only one program can hold the serial port. Two at once can wedge the USB device until you replug it.
- If the watch last ran an OTA-installed image, `otadata` still points at the other slot and a USB flash into
  `app0` keeps booting the old one: run `espflash erase-region 0xe000 0x2000` first (this keeps NVS and your saved
  Wi-Fi; `erase-flash` clears it).

## Tests

The logic worth testing lives in `crates/putting-core` and runs on the host:

```sh
cd crates/putting-core && cargo test --target aarch64-apple-darwin   # use your host triple
```

That is the swing tracker (16 tests, ported one-for-one from the C++ suite), the updater logic (17, likewise),
the Wi-Fi portal helpers and the update-screen mapping. The firmware itself is tested on hardware.

## Compatibility with the C++ firmware

A partition table and a bootloader can't be changed over the air, so a Rust image has to load under whatever is
already on a watch. Hence `partitions.csv` is the C++ table byte for byte, and `sdkconfig.defaults` sets what the
C++ bootloader expects: 16 MB flash, **64 KB MMU pages**, and **app rollback enabled**. With rollback on, a freshly
installed image boots as "pending verify" and the bootloader reverts it at the next reset unless the app calls
`esp_ota_mark_app_valid_cancel_rollback()` — `confirm_firmware()` in `main.rs` does that once the IMU, codec and
display are up (and rolls back immediately if the IMU is missing). Wi-Fi credentials are saved in the same NVS
namespace (`wifi`) the C++ updater uses, so they survive a switch between the two.

## Releasing

**CI does it.** Push a tag `vX.Y.Z` (or `vX.Y.Z-beta.1`) whose version equals `version` in `Cargo.toml`
(the firmware reports that as its running version, and the workflow refuses a mismatch). The Release workflow
(`.github/workflows/release.yml`) runs the host tests, builds with no Wi-Fi credentials, makes the images, checks
the app image fits the 5 MiB OTA slot, creates a GitHub Release with both images, and *last* publishes the site
on `gh-pages`: `manifest.json` (the over-the-air contract), the firmware, the USB-install page and the guide. A
push to `main` only builds and tests. `docs.yml` republishes the guide alone when `docs/` changes on `main`. Both
workflows use only the built-in `GITHUB_TOKEN`. The first run is slow (it builds ESP-IDF); later ones use caches.

The steps below are what the workflow does, for doing it by hand (for instance to publish a test image).
The C++ firmware's releases go to the same site; only `manifest.json` and `firmware/<version>/firmware.bin` need
to change for an over-the-air release.

1. **Version.** Set `version` in `Cargo.toml`. Use a prerelease of the *next* C++ version, e.g. `0.1.6-rust.1`: it
   is newer than every released C++ build but older than `0.1.6`, so the next C++ release supersedes it with no
   manual revoking. The running version the updater compares comes from `CARGO_PKG_VERSION`.
2. **Build** with the Wi-Fi variables unset: `cargo build --release`.
3. **Images.** The OTA file is a bare app image, the USB installer wants a merged one:
   ```sh
   BL=target/riscv32imac-esp-espidf/release/build/esp-idf-sys/*/out/build/bootloader/bootloader.bin
   ELF=target/riscv32imac-esp-espidf/release/putting-watch-rs
   espflash save-image --chip esp32c6 --flash-size 16mb --partition-table partitions.csv --bootloader $BL $ELF firmware.bin
   espflash save-image --chip esp32c6 --flash-size 16mb --partition-table partitions.csv --bootloader $BL --merge $ELF firmware.factory.bin
   ```
4. **Manifest.** Run PuttingWatch's `tools/make_release_site.py` on them (it writes the manifest with the size and
   SHA-256 and does the same sanity checks as the C++ release), and publish `manifest.json` plus
   `firmware/<version>/firmware.bin` to the `gh-pages` branch.

A manifest is a contract with devices already in the field: only ever add optional fields. A device never
downgrades by version number, so to move a watch back, publish a higher version (or flash over USB).

### Testing rollback

`cargo build --release --features crash_on_boot` builds an image that panics at the start of `main`, before it can
confirm itself. Publish it as a prerelease, install it over the air on a spare watch, and watch the serial console:
it panics, resets, and the bootloader reverts to the previous image. Never ship it; revert the manifest afterwards.

## What the port taught us

- **Toolchain:** nightly + `build-std` is mandatory, and a nightly can break std for ESP-IDF between days. Pin it.
- **Drivers:** the safe `esp-idf-hal` wrappers (I2C, GPIO, I2S, Wi-Fi) work well; anything without one (`esp_lcd`,
  the display) is called through `esp-idf-sys` FFI. The APIs moved while we used them (`hal::prelude`,
  `Peripheral`, `PinDriver` generics all changed).
- **Threads are FreeRTOS tasks.** On this single-core part, priorities matter: sampling above everything, the
  audio feeder above the display thread (a full-screen repaint starved I2S and "ready" came out as "Read-dy"), and
  the display thread below the update engine so repaints can't slow a download.
- **I2S** needs `auto_clear` on, or it replays its last DMA buffer forever.
- **SNTP:** `sntp_get_sync_status()` reports `Completed` once and then resets; judge the clock by its value.
- **QMI8658** burst reads need register auto-increment (CTRL1 bit 6) set explicitly; don't rely on state left by
  earlier firmware.
- **Binary size:** the whole app with Wi-Fi, TLS and the spoken samples is about 2.7 MB (release), well inside the
  5 MB OTA slots.

## Not ported / differences

- The C++ app drops the CPU to 80 MHz while running; this port stays at 160 MHz.
- The updater's JSON is parsed with `serde_json` (non-ASCII in the notes text becomes `?`, as the C++ did for
  `\u` escapes).
