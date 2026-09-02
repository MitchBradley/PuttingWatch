#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One spoken cue sample: 16 kHz mono 16-bit PCM (see Clicker::kSampleRate).
typedef struct {
    const int16_t* data;
    int frames;  // 0 = not compiled in -> Clicker::playCue() is a silent no-op
} CueSample;

// Named one-shot voice cues. Keep this in sync with CUES in
// tools/wav2c_cues.py (that generator emits kCues[] entries by these names).
// Regenerate src/assets/cues_audio.c after updating assets/*.wav.
enum CueId {
    CUE_READY = 0,  // re-armed for the next stroke (assets/ready.wav)
    CUE_COUNT
};

extern const CueSample kCues[];  // indexed by CueId; CUE_COUNT entries

#ifdef __cplusplus
}
#endif
