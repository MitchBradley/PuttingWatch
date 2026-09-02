#pragma once
#include <Arduino.h>

// Feedback renderer. Pure output: no swing logic - it is told "click" or
// "beep N times" and nothing more (the Model/renderer split from the Swift
// app; keeping detection logic out of here was a real bug fix there).
//
// Drives the onboard speaker via the ES8311 codec (I2S) + NS4150B amp. A
// short pre-rendered sine burst per click; a longer tone for the
// calibration cue. Playback runs on its own FreeRTOS task so a blocking
// i2s.write() never stalls the UI loop; playClick()/playBeep() just enqueue.
//
// If begin() can't bring the codec up (not ACKing on I2C, etc.) it returns
// false and every later call is a silent no-op - audio is optional, the
// firmware still runs.
class Clicker {
  public:
    bool begin();

    void playClick();      // motion click - short, high
    void playTick();       // UI button confirm - short, lower pitch than the click
    void playBeep(int n);  // n longer tones - calibration cue

    void tick() {}  // kept for API compatibility; playback is task-driven now

  private:
    void renderTone(int16_t* dst, int nFrames, float freqHz, float amp, int rampFrames);
    static void audioTaskThunk(void* self) { static_cast<Clicker*>(self)->audioTask(); }
    void audioTask();

    bool ok_ = false;
    void* es_ = nullptr;  // es8311_handle_t

    static constexpr int kSampleRate = 16000;
    static constexpr int kClickMs = 14;
    static constexpr int kBeepMs = 110;
    static constexpr float kClickHz = 2400.0f;  // motion click
    static constexpr float kTickHz = 1150.0f;   // UI button - ~1 octave lower
    static constexpr float kBeepHz = 880.0f;    // calibration cue

    int16_t* clickBuf_ = nullptr;  // interleaved stereo
    int16_t* tickBuf_ = nullptr;
    int16_t* beepBuf_ = nullptr;
    int clickFrames_ = 0;
    int tickFrames_ = 0;
    int beepFrames_ = 0;

    QueueHandle_t queue_ = nullptr;  // of uint8_t: 0 = click, 1 = beep, 2 = tick
};
