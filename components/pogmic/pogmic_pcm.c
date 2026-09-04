#include "pogmic_pcm.h"

#include <math.h>

pogmic_levels_t pogmic_pcm_measure(const int32_t *stereo, size_t words) {
  pogmic_levels_t levels = {.dbfs = {-96.0f, -96.0f}};
  if (!stereo || words < 2) {
    return levels;
  }
  levels.frames = words / 2;
  double sum[2] = {0, 0};
  for (size_t i = 0; i < levels.frames; i++) {
    for (size_t slot = 0; slot < 2; slot++) {
      int32_t raw = stereo[2 * i + slot];
      double value = raw;
      sum[slot] += value * value;
      levels.nonzero[slot] += raw != 0;
    }
  }
  for (size_t slot = 0; slot < 2; slot++) {
    if (sum[slot] > 0) {
      float db = (float)(10.0 * log10(sum[slot] / levels.frames /
                                      (2147483648.0 * 2147483648.0)));
      levels.dbfs[slot] = db < -96.0f ? -96.0f : db;
    }
  }
  return levels;
}

void pogmic_pcm_expand(const int16_t *pcm, int32_t *wide, size_t samples) {
  for (size_t i = 0; i < samples; i++) {
    /* Multiplication is defined for negative samples, unlike signed shifts. */
    wide[i] = (int32_t)pcm[i] * 65536;
  }
}

bool pogmic_shared_pins_valid(int bclk, int ws, int data, int speaker_bclk,
                              int speaker_ws, int speaker_data,
                              uint64_t outputs) {
  if (speaker_data < 0 || speaker_data >= 64 ||
      !(outputs & (UINT64_C(1) << speaker_data))) {
    return false;
  }
  return bclk >= 0 && ws >= 0 && data >= 0 && bclk != ws && data != bclk &&
         data != ws && bclk == speaker_bclk && ws == speaker_ws &&
         speaker_data != bclk && speaker_data != ws && speaker_data != data;
}

size_t pogmic_pcm_decode(const int32_t *stereo, size_t words, bool right,
                         int16_t *mono, size_t capacity) {
  if (!stereo || !mono) {
    return 0;
  }
  size_t count = words / 2;
  if (count > capacity) {
    count = capacity;
  }
  for (size_t i = 0; i < count; i++) {
    /* Unsigned extraction avoids relying on right-shift of negative signed
     * values; subtraction expresses sign extension portably on host too. */
    uint32_t top = (uint32_t)stereo[2 * i + (right ? 1 : 0)] >> 16;
    mono[i] = (int16_t)(top >= 32768 ? (int32_t)top - 65536 : (int32_t)top);
  }
  return count;
}

float pogmic_pcm_dbfs(const int16_t *mono, size_t count) {
  if (!mono || count == 0) {
    return -96.0f;
  }
  double sum = 0;
  for (size_t i = 0; i < count; i++) {
    double v = mono[i];
    sum += v * v;
  }
  if (sum == 0) {
    return -96.0f;
  }
  float result = (float)(10.0 * log10(sum / count / (32768.0 * 32768.0)));
  return result < -96.0f ? -96.0f : result;
}

bool pogmic_pins_valid(int bclk, int ws, int data, uint64_t inputs,
                       uint64_t outputs, uint64_t occupied) {
  if (bclk < 0 || bclk >= 64 || ws < 0 || ws >= 64 || data < 0 || data >= 64 ||
      bclk == ws || bclk == data || ws == data) {
    return false;
  }
  uint64_t b = UINT64_C(1) << bclk;
  uint64_t w = UINT64_C(1) << ws;
  uint64_t d = UINT64_C(1) << data;
  return (outputs & b) && (outputs & w) && (inputs & d) &&
         !(occupied & (b | w | d));
}
