#include "pogvoice_feedback.h"

bool pogvoice_feedback_render(uint8_t *rgb, size_t count,
                              pogvoice_light_t state, uint32_t elapsed_ms,
                              float level) {
  if (!rgb || !count || state <= POGVOICE_LIGHT_OFF ||
      state > POGVOICE_LIGHT_ERROR ||
      (state == POGVOICE_LIGHT_ERROR && elapsed_ms >= 4000))
    return false;
  if (!(level >= 0))
    level = 0;
  if (level > 1)
    level = 1;
  /* Slow triangular breathing: no strobe and no per-pixel trigonometry. */
  uint32_t phase = elapsed_ms % 1800;
  uint32_t pulse = phase < 900 ? phase : 1800 - phase;
  uint8_t r = 0, g = 0, b = 0;
  unsigned intensity = 70 + pulse * 100 / 900;
  switch (state) {
  case POGVOICE_LIGHT_CONNECTING:
    r = 140;
    g = 80;
    b = 255;
    break;
  case POGVOICE_LIGHT_LISTENING:
    g = 180;
    b = 255;
    break;
  case POGVOICE_LIGHT_THINKING:
    r = 255;
    g = 150;
    b = 20;
    break;
  case POGVOICE_LIGHT_SPEAKING:
    r = 35;
    g = 255;
    b = 110;
    break;
  case POGVOICE_LIGHT_ERROR:
    r = 255;
    g = 25;
    b = 15;
    break;
  default:
    return false;
  }
  for (size_t i = 0; i < count; i++) {
    unsigned value = intensity;
    if (state == POGVOICE_LIGHT_LISTENING || state == POGVOICE_LIGHT_SPEAKING) {
      size_t distance =
          (2 * i + 1 > count) ? 2 * i + 1 - count : count - (2 * i + 1);
      value = distance <= (size_t)(level * count) ? 220 : 35 + pulse * 35 / 900;
    }
    rgb[3 * i] = r * value / 255;
    rgb[3 * i + 1] = g * value / 255;
    rgb[3 * i + 2] = b * value / 255;
  }
  return true;
}
