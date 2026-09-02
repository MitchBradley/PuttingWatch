#include "Clicker.h"
#include <ESP_I2S.h>
#include <Wire.h>
#include <math.h>
#include "pin_config.h"

extern "C" {
#include "es8311.h"
}

namespace {
I2SClass i2s;

// Codec bring-up, modelled on Waveshare's 08_ES8311 example but returning a
// bool instead of ESP_ERROR_CHECK-aborting, so a missing/miswired codec just
// disables audio.
void* bringUpCodec(int sampleRate, int volume) {
    es8311_handle_t h = es8311_create(0 /* I2C port - same as Wire */, ES8311_ADDRESS_0);
    if (!h) return nullptr;

    const es8311_clock_config_t clk = {
        .mclk_inverted = false,
        .sclk_inverted = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency = sampleRate * 256,
        .sample_frequency = sampleRate,
    };
    if (es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
        es8311_delete(h);
        return nullptr;
    }
    es8311_sample_frequency_config(h, clk.mclk_frequency, clk.sample_frequency);
    es8311_microphone_config(h, false);
    es8311_voice_volume_set(h, volume, nullptr);
    return h;
}
}  // namespace

// Sine burst with a raised-cosine attack/release so it doesn't pop.
void Clicker::renderTone(int16_t* dst, int nFrames, float freqHz, float amp, int rampFrames) {
    const float w = 2.0f * (float)M_PI * freqHz / kSampleRate;
    for (int i = 0; i < nFrames; i++) {
        float env = 1.0f;
        if (i < rampFrames) env = 0.5f - 0.5f * cosf((float)M_PI * i / rampFrames);
        else if (i > nFrames - rampFrames)
            env = 0.5f - 0.5f * cosf((float)M_PI * (nFrames - i) / rampFrames);
        int16_t s = (int16_t)(sinf(w * i) * env * amp * 32767.0f);
        dst[2 * i] = s;      // L
        dst[2 * i + 1] = s;  // R
    }
}

bool Clicker::begin() {
    pinMode(PA_CTRL, OUTPUT);
    digitalWrite(PA_CTRL, LOW);  // amp off until the codec is up

    i2s.setPins(I2S_SCLK, I2S_LRCK, I2S_DSDIN, I2S_ASDOUT, I2S_MCLK);
    if (!i2s.begin(I2S_MODE_STD, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                   I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        return false;
    }

    es_ = bringUpCodec(kSampleRate, /*volume 0-100*/ 80);
    if (!es_) {
        i2s.end();
        return false;
    }

    clickFrames_ = kSampleRate * kClickMs / 1000;
    beepFrames_ = kSampleRate * kBeepMs / 1000;
    clickBuf_ = (int16_t*)malloc(clickFrames_ * 2 * sizeof(int16_t));
    beepBuf_ = (int16_t*)malloc(beepFrames_ * 2 * sizeof(int16_t));
    if (!clickBuf_ || !beepBuf_) return false;

    const int ramp = kSampleRate * 2 / 1000;  // 2 ms
    renderTone(clickBuf_, clickFrames_, 2400.0f, 0.6f, ramp);
    renderTone(beepBuf_, beepFrames_, 880.0f, 0.5f, ramp);

    digitalWrite(PA_CTRL, HIGH);  // amp on, stays on

    queue_ = xQueueCreate(8, sizeof(uint8_t));
    xTaskCreatePinnedToCore(audioTaskThunk, "audio", 3072, this, 1, nullptr, 0);

    ok_ = true;
    return true;
}

void Clicker::playClick() {
    if (!ok_) return;
    uint8_t k = 0;
    xQueueSend(queue_, &k, 0);  // drop if the queue is backed up
}

void Clicker::playBeep(int n) {
    if (!ok_) return;
    uint8_t k = 1;
    for (int i = 0; i < n; i++) xQueueSend(queue_, &k, 0);
}

void Clicker::audioTask() {
    uint8_t item;
    for (;;) {
        if (xQueueReceive(queue_, &item, portMAX_DELAY) != pdTRUE) continue;
        if (item == 0) {
            i2s.write((uint8_t*)clickBuf_, clickFrames_ * 2 * sizeof(int16_t));
        } else {
            i2s.write((uint8_t*)beepBuf_, beepFrames_ * 2 * sizeof(int16_t));
            vTaskDelay(pdMS_TO_TICKS(60));  // gap between calibration beeps
        }
    }
}
