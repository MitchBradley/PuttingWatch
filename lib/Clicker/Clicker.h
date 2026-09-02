#pragma once
#include <Arduino.h>

// Feedback renderer. Pure output: no swing logic of its own - it is told
// "click" or "beep N times" and nothing more (the Model/renderer split from
// the Swift app; keeping detection logic out of here was a real bug fix
// there).
//
// This board has NO confirmed vibration motor and driving the ES8311 codec
// for a 15 ms tick is a lot of setup, so the first cut pulses GPIO18 - the
// pin the community board template labels "haptic motor". If GPIO18 turns out
// to be unconnected on your unit, this is silent and harmless; wire the
// ES8311 path in playTone_() then (see plan M3).
//
// Non-blocking: playClick()/playBeep() only enqueue; tick() must be called
// often from loop() to drive the pin edges.
class Clicker {
  public:
    void begin();

    void playClick();        // one short pulse
    void playBeep(int n);    // n pulses, ~calibration cue

    void tick();             // call every loop()

  private:
    void setMotor_(bool on);

    static constexpr uint32_t kPulseOnMs = 12;
    static constexpr uint32_t kPulseGapMs = 55;

    int pending_ = 0;
    bool motorOn_ = false;
    uint32_t edgeDueMs_ = 0;
};
