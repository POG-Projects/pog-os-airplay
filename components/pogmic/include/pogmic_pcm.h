#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 32-bit stereo I2S words -> signed 16-bit mono; trailing partial frame is
 * deliberately ignored. No gain or AGC is applied to the capture. */
size_t pogmic_pcm_decode(const int32_t *stereo, size_t words, bool right,
                         int16_t *mono, size_t capacity);
float pogmic_pcm_dbfs(const int16_t *mono, size_t count);

typedef struct {
  size_t frames;
  float dbfs[2];       /* left, right; floor -96 dBFS */
  uint32_t nonzero[2]; /* raw I2S words, before PCM16 truncation */
} pogmic_levels_t;

/* Measure both slots from the same capture without discarding low bits. */
pogmic_levels_t pogmic_pcm_measure(const int32_t *stereo, size_t words);

/* Preserve PCM amplitude when the speaker bus uses signed 32-bit words. */
void pogmic_pcm_expand(const int16_t *pcm, int32_t *wide, size_t samples);

bool pogmic_shared_pins_valid(int bclk, int ws, int data, int speaker_bclk,
                              int speaker_ws, int speaker_data,
                              uint64_t outputs);

/* The caller supplies the target's GPIO capabilities and every occupied pin.
 * Reserved flash/PSRAM pins must be included in occupied. */
bool pogmic_pins_valid(int bclk, int ws, int data, uint64_t inputs,
                       uint64_t outputs, uint64_t occupied);
