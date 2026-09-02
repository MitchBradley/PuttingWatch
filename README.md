# PuttingWatch

Golf putting-stroke trainer for the **Waveshare ESP32-S3-Touch-AMOLED-2.06**
watch devkit. A C++/PlatformIO port of the `PuttingCoachWatch` watchOS app
(itself the wrist-worn subset of the `PuttingCoach` iOS project).

The device is mounted on the putter. You tap to calibrate a rest orientation,
then it gives a **click** (a short haptic pulse) each time the backswing
passes a new angular threshold. Clicking stops on the reversal into the
forward stroke and re-arms once the putter settles back near address.

This first port covers the *watch subset only*: calibration + backswing/click
detection. Impact detection and face-angle-at-impact (from the phone app) are
future work — see `~/.claude/plans/shimmying-finding-whale.md`.

## Layout

| Path | What | Verified |
|---|---|---|
| `lib/PuttingCore/` | `SwingTracker` (the Model — all swing logic), `GravityFilter`, `MotionSample`, `Vec3`. No Arduino deps. | ✅ host unit tests |
| `lib/Imu/` | QMI8658 driver wrapper (lewisxhe/SensorLib) | ⏳ needs board |
| `lib/Clicker/` | Feedback renderer — pulses GPIO18; ES8311 audio path is a TODO | ⏳ needs board |
| `src/ui.*` | LVGL 9 view: state-mapped screens, background tint, click-interval slider | 🔨 compiles; runtime needs board |
| `src/main.cpp` | Board bring-up + wiring; 100 Hz sampling task on core 1, LVGL on core 0 | 🔨 compiles; runtime needs board |
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

1. Screen shows "Tap when in putting position" on a muted-olive background;
   a finger touch registers.
2. Serial prints `state=… phase=… ready=… angle=… clicks=…` once a second.
3. Tap → "Hold still…" (yellow) for ~2 s → rest x/y/z shown → live angle
   screen.
4. Hold the board level, then tilt it about one axis: the angle number
   tracks, and a click (GPIO18 pulse) fires every `click:` degrees.
5. Tilt past ~a few degrees then bring it back: clicks stop after the
   reversal; background tint drops from green; it re-arms after ~2 s held
   still near the calibrated rest.
6. Drag the slider — click spacing changes (2–12°). "Recal" restarts
   calibration; "Stop" pauses sampling.

### Known open items

- **GPIO18 feedback pin** — the community board template labels it a haptic
  motor; the Waveshare spec sheet does not list one. If nothing is felt,
  wire up the ES8311 audio path in `lib/Clicker/Clicker.cpp`.
- **`GravityFilter` blend** and the `SwingTracker` constants are ported from
  the Swift app as starting points; expect to re-tune them on-device against
  real putter-mounted strokes.
