# PuttingWatch

Golf putting-stroke trainer for the **Waveshare ESP32-S3-Touch-AMOLED-2.06**
watch devkit. A C++/PlatformIO port of the `PuttingCoachWatch` watchOS app
(itself the wrist-worn subset of the `PuttingCoach` iOS project).

The device is mounted on the putter. You tap to calibrate a rest orientation,
then it gives an audible **click** (a short tone through the onboard speaker)
each time the backswing passes a new angular threshold. Clicking stops on the
reversal into the forward stroke and re-arms once the putter settles back
near address.

This first port covers the *watch subset only*: calibration + backswing/click
detection. Impact detection and face-angle-at-impact (from the phone app) are
future work — see `~/.claude/plans/shimmying-finding-whale.md`.

## Layout

| Path | What | Verified |
|---|---|---|
| `lib/PuttingCore/` | `SwingTracker` (the Model — all swing logic), `GravityFilter`, `MotionSample`, `Vec3`. No Arduino deps. | ✅ host unit tests |
| `lib/Imu/` | QMI8658 driver wrapper (lewisxhe/SensorLib) | ⏳ needs board |
| `lib/Clicker/` | Feedback renderer — ES8311 codec + speaker over I2S, playback on its own task | 🔨 compiles; runtime needs board |
| `lib/Es8311/` | Vendored ES8311 codec driver (Espressif/Waveshare) | — |
| `lib/Battery/` | AXP2101 battery-percentage readout (XPowersLib) | 🔨 compiles; runtime needs board |
| `src/ui.*` | LVGL 9 view: state-mapped screens, background tint, click-interval slider | 🔨 compiles; runtime needs board |
| `src/main.cpp` | Board bring-up + wiring; 100 Hz sampling task on core 1, LVGL on core 0 | 🔨 compiles; runtime needs board |
| `assets/Honu.png` | Waiting-screen image, original art | — |
| `src/assets/Honu.c`, `src/assets/HonuBottom.c` | GIMP RGB565 C-source exports, compiled directly, each with a small documented wrapper (`honu_img` = waiting screen, `honu_bottom_img` = "Hold still" screen; ~305 KB flash each, no runtime conversion). `src/honu_img.h` declares both. | — |
| `assets/{one..twenty_five}.wav` | Piper-synthesised spoken numbers (female voice) — see [Regenerating spoken-number audio](#regenerating-spoken-number-audio-piper). | — |
| `src/assets/numbers_audio.c` | Generated from the WAVs by `tools/wav2c_numbers.py` (16 kHz mono 16-bit, trimmed/normalised). `Clicker::playNumber()` speaks the value on `−`/`+` and after a swing completes. Regenerate after editing the WAVs. | — |
| `lib/Arduino_DriveBus/` | Vendored Waveshare FT3168 touch driver (not on the PlatformIO registry) | — |
| `include/pin_config.h` | Full board pin map | — |
| `include/lv_conf.h` | LVGL 9.3 config (from Waveshare's examples, demos off) | — |

## Architecture

Strict Model / renderer / wiring split, carried over from the Swift app:

- **`SwingTracker`** owns *all* stroke logic. It has no idea LVGL or audio
  exist. Driven by `update(MotionSample)` at a fixed rate; all timing comes
  from `sample.t_ms`.
- **`Clicker`** is a pure renderer: `playClick()` / `playBeep(n)`, no logic.
- **`ui` / `main`** wire Model outputs (`state()`, `isReadyForSwing()`,
  `onClick`) to the screen and the renderer.

Do not put swing math in the UI or audio layers — that split was a real bug
source in the Swift version.

## Build & test

Host unit tests for the swing logic (no board needed):

```bash
pio test -e native
```

Firmware:

```bash
pio run -e esp32s3 -t upload -t monitor
```

## Regenerating spoken-number audio (Piper)

`assets/{one..twenty_five}.wav` are synthesised with
[Piper](https://github.com/OHF-Voice/piper1-gpl) using the
`en_US-hfc_female-medium` voice. The model (`en_US-hfc_female-medium.onnx`,
~63 MB) and its `en_US-hfc_female-medium.onnx.json` config live in the repo
root alongside the project files, not in `assets/` — that's where Piper looks
for them by default.

Install Piper (once):

```bash
pip install piper-tts
```

Synthesise a new number (run from the repo root, so Piper finds the model):

```bash
echo "twenty six" | python3 -m piper -m en_US-hfc_female-medium.onnx -f assets/twenty_six.wav
```

(`python3 -m piper` works even if the `piper` command itself isn't on `PATH`.)

Preview a WAV before compiling it in:

```bash
afplay assets/twenty_six.wav
```

Then add the new word to `WORDS` in `tools/wav2c_numbers.py` (spelled-out word
→ integer, matching the `.wav` filename) and regenerate:

```bash
python3 tools/wav2c_numbers.py
```

That rewrites `src/assets/numbers_audio.c`. `Clicker::maxSpokenNumber()`
reads the new count directly, so nothing else needs to change for a bigger
number to become speakable.

## Releasing firmware

Pushing a version tag builds, tests and publishes a release
(`.github/workflows/release.yml`). The version number comes from the tag alone
(`tools/fw_version.py` turns `git describe` into `FW_VERSION`) - there is
nothing in the source to bump.

**One-time GitHub setup** (the workflow can't do these):

1. Settings → Pages → Source: **GitHub Actions**.
2. Settings → Environments → `github-pages` → Deployment branches and tags →
   add a tag rule `v*`. By default only the default branch may deploy to that
   environment, which rejects a tag-triggered deploy.

**To release:**

```bash
git tag -a v0.1.0 -m "One-line summary, shown on the install page"
git push origin v0.1.0
```

Tags must look like `v1.2.3` or `v1.2.3-beta.1` (a `-` marks a GitHub
pre-release). The workflow runs the unit tests, builds `esp32c6`, checks the
firmware's own version string equals the tag, then publishes:

| What | Where | For |
|---|---|---|
| `firmware-<ver>.factory.bin` | GitHub Release asset | full image (bootloader + partitions + app), USB install |
| `firmware-<ver>.bin` | GitHub Release asset | app image only; permanent archive of every release |
| `https://<owner>.github.io/PuttingWatch/` | GitHub Pages | USB install page (ESP Web Tools: Chrome/Edge + cable) |
| `.../manifest.json` | GitHub Pages | what the watch reads to find the newest update |
| `.../firmware/<ver>/firmware.bin` | GitHub Pages | the image the watch downloads |

Pages only ever holds the newest release. "Run workflow" on the Actions tab
builds and tests without publishing - a quick way to check CI is healthy.

`manifest.json` carries `version`, `url`, `size`, `sha256`, `min_version`,
`released` and `notes`. Watches in the field depend on it, so **only ever add
optional fields to it - never rename or repurpose one.**

`release/min_ota_version.txt` becomes the manifest's `min_version`: the oldest
installed firmware allowed to update over the air. Raise it for any release
that can't be applied over the air to older firmware - above all when the
partition table changes, which only a USB install can fix.

Preview the install page locally (no GitHub needed):

```bash
python3 tools/make_release_site.py --version 0.1.0 --base-url http://localhost:8000/ \
  --firmware .pio/build/esp32c6/firmware.bin --factory .pio/build/esp32c6/firmware.factory.bin \
  --template release/index.template.html --out /tmp/site && cd /tmp/site && python3 -m http.server 8000
```

## On-device bring-up checklist (first flash)

1. Screen shows the Honu image centred on white with "Tap when in putting
   position" (black) in the band above it; a finger touch registers.
2. Serial prints `state=… phase=… ready=… angle=… clicks=…` once a second.
3. Tap → "Hold still…" (yellow) for ~2 s → rest x/y/z shown → live angle
   screen. A battery bar + percentage sits below the top buttons (shows
   "--" if the AXP2101 reports no battery, e.g. USB-only).
4. Hold the board level, then tilt it about one axis: the angle number
   tracks, and an audible click fires every `click:` degrees. (Two short
   tones also play when calibration completes.)
5. Tilt past ~a few degrees then bring it back: clicks stop after the
   reversal; background tint drops from green; it re-arms after ~2 s held
   still near the calibrated rest.
6. `−` / `+` change click spacing (2–12°) and speak the new value ("four"…
   "twelve"); rapid presses speak only the final value. "Recal" restarts
   calibration.
7. **Sleep:** a tap on empty screen area (not a button) → *light sleep*
   (~1–2 mA, display + amp off); touch the screen or press BOOT to wake and
   resume exactly where it was. **"Stop" → *deep sleep*** (~10 µA, RAM lost);
   only the BOOT button wakes it, via a full reboot to the tap-to-start
   screen. (BOOT is also the download-mode strap — a quick press wakes; don't
   hold it.)

### Known open items

- **Audio** — ES8311 codec + NS4150B amp (PA enable GPIO46), 16 kHz mono
  tones, playback on a dedicated FreeRTOS task. If the codec doesn't come up
  the firmware logs "audio disabled" and runs silently. Volume, tone pitch,
  and durations are constants at the top of `lib/Clicker/Clicker.{h,cpp}`.
- **`GravityFilter` blend** and the `SwingTracker` constants are ported from
  the Swift app as starting points; expect to re-tune them on-device against
  real putter-mounted strokes.
