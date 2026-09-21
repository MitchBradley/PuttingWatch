#pragma once
#include <Arduino.h>
#include "cues_audio.h"  // CueId (CUE_READY, ...)

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
    void playNumber(int n);   // spoken number (whatever's compiled in); tone fallback
    int maxSpokenNumber() const;  // highest n with a compiled-in sample; callers clamp to this
    void playCue(uint8_t id);  // named voice cue (CueId, e.g. CUE_READY); no-op if absent

    // NS4150B power-amp enable. Kept off except when sounds can actually play
    // (the run state) to save its few-mA quiescent draw. Enabling blocks
    // briefly so the amp is out of shutdown before the first sample.
    void setAmpEnabled(bool on);

    void tick() {}  // kept for API compatibility; playback is task-driven now

  private:
    void renderTone(int16_t* dst, int nFrames, float freqHz, float amp, int rampFrames);
    // Square wave swept linearly from freqStartHz to freqEndHz - more
    // harmonic content than a pure tone (harder to mask outdoors) and a
    // sweep doesn't depend on any single frequency not being masked.
    void renderChirpSquare(int16_t* dst, int nFrames, float freqStartHz, float freqEndHz,
                           float amp, int rampFrames);
    // mono PCM -> stereo I2S, chunked. gain > 1.0 hard-clips (intentional,
    // for cues that need to be louder than their source sample allows).
    void streamMono(const int16_t* data, int frames, float gain = 1.0f);
    static void audioTaskThunk(void* self) { static_cast<Clicker*>(self)->audioTask(); }
    void audioTask();

    bool ok_ = false;
    bool ampOn_ = false;
    static constexpr int kAmpSettleMs = 10;  // NS4150B shutdown-release before audio
    void* es_ = nullptr;  // es8311_handle_t

    static constexpr int kSampleRate = 16000;
    static constexpr int kClickMs = 50;  // was 14 - some on-course clicks weren't heard at all
    static constexpr int kTickMs = 14;    // UI button confirm - unaffected, stays short
    static constexpr int kBeepMs = 110;
    static constexpr float kClickHz = 2400.0f;  // motion click - EXPERIMENT (previous): see below
    static constexpr float kTickHz = 1150.0f;   // UI button - ~1 octave lower
    static constexpr float kBeepHz = 880.0f;    // calibration cue

    // Click sweeps a square wave 1.5 kHz -> 2.5 kHz instead of a pure tone -
    // a square wave's harmonics and a moving frequency are both harder to
    // mask outdoors than one fixed sine. See renderChirpSquare().
    static constexpr float kClickChirpStartHz = 1500.0f;
    static constexpr float kClickChirpEndHz = 2500.0f;

    // Loud on purpose, clipping intentionally - the click (backswing
    // threshold) and Ready (re-armed) cues need to cut through outdoors, and
    // being heard matters more than being clean. Square-wave click is
    // already full-scale in its steady region at amp=1.0 (going higher only
    // hardens the anti-pop ramp, it can't get any louder) - kClickAmp still
    // applies if the sine click is restored below.
    static constexpr float kClickAmp = 1.6f;     // vs. tick/beep's 0.6/0.5 - unclipped
    static constexpr float kReadyCueGain = 2.0f;  // digital gain on assets/ready.wav

    int16_t* clickBuf_ = nullptr;  // interleaved stereo
    int16_t* tickBuf_ = nullptr;
    int16_t* beepBuf_ = nullptr;
    int clickFrames_ = 0;
    int tickFrames_ = 0;
    int beepFrames_ = 0;

    // Scratch for streaming a mono number sample out as stereo.
    static constexpr int kStereoChunkFrames = 256;
    int16_t* stereoScratch_ = nullptr;

    // uint8_t queue: 0 = click, 1 = beep, 2 = tick; a spoken number n is
    // queued as kNumberQueueBase + n (numbers can be 1/2, which would
    // otherwise collide with the tone codes); a voice cue id is queued as
    // kCueQueueBase + id.
    static constexpr uint8_t kNumberQueueBase = 100;
    static constexpr uint8_t kCueQueueBase = 200;
    QueueHandle_t queue_ = nullptr;
};
