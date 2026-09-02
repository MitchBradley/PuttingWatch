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
| `assets/{four..twelve}.wav` | Piper-synthesised spoken click-interval values (female voice). | — |
| `src/assets/numbers_audio.c` | Generated from the WAVs by `tools/wav2c_numbers.py` (16 kHz mono 16-bit, trimmed/normalised, ~124 KB flash). `Clicker::playNumber()` speaks the value on `−`/`+`. Regenerate after editing the WAVs. | — |
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
