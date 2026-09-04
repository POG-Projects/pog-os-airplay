#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
  POGVOICE_LIGHT_OFF,
  POGVOICE_LIGHT_CONNECTING,
  POGVOICE_LIGHT_LISTENING,
  POGVOICE_LIGHT_THINKING,
  POGVOICE_LIGHT_SPEAKING,
  POGVOICE_LIGHT_ERROR,
} pogvoice_light_t;

/* Transient overlay only: never changes the lamp's saved settings. */
bool pogvoice_feedback_render(uint8_t *rgb, size_t count,
                               pogvoice_light_t state, uint32_t elapsed_ms,
                               float level);
