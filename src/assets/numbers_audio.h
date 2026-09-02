#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One spoken number sample: 16 kHz mono 16-bit PCM (see Clicker::kSampleRate).
typedef struct {
    const int16_t* data;
    int frames;  // 0 = not available -> caller falls back to a tone
} NumberSample;

// Index by click-threshold degrees. Valid indices are 0 .. kNumberSamplesCount-1;
// entries with no WAV have frames == 0. Regenerate src/assets/numbers_audio.c
// with tools/wav2c_numbers.py after updating assets/*.wav.
extern const NumberSample kNumberSamples[];
extern const int kNumberSamplesCount;

#ifdef __cplusplus
}
#endif
