#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EFFECT_SYNC_CAPACITY   8
#define EFFECT_SYNC_TIMEOUT_MS 500

typedef enum {
  EFFECT_VISUALIZER_SPECTRUM,
  EFFECT_VISUALIZER_VU_METER,
  EFFECT_VISUALIZER_BASS_PULSE,
  EFFECT_VISUALIZER_RAINBOW,
} effect_visualizer_t;

typedef struct {
  float hue;
  float saturation;
  float value;
} effect_sync_pixel_t;

typedef struct {
  uint32_t seq;
  uint64_t mono_ms;
  uint64_t present_at_ms;
  uint16_t lead_ms;
  float level;
  float bass;
  float treble;
  uint32_t received_ms;
} effect_sync_frame_t;

typedef struct {
  bool joined;
  char group_id[37];
  char leader_entity_id[37];
  uint16_t presentation_delay_ms;
  int16_t calibration_offset_ms;
  effect_visualizer_t visualizer;
  effect_sync_frame_t frames[EFFECT_SYNC_CAPACITY];
  size_t frame_count;
  bool has_rendered;
  uint32_t last_rendered_seq;
} effect_sync_t;

bool effect_sync_uuid_is_canonical(const char *value);
bool effect_sync_visualizer_from_name(const char *value,
                                      effect_visualizer_t *out);
float effect_sync_pixel_position(size_t index, size_t count);
effect_sync_pixel_t
effect_sync_visualizer_pixel(effect_visualizer_t visualizer, float position,
                             const effect_sync_frame_t *frame);
bool effect_sync_join(effect_sync_t *state, const char *group_id,
                      const char *role, const char *leader_entity_id,
                      int presentation_delay_ms, int calibration_offset_ms,
                      const char *visualizer);
bool effect_sync_leave(effect_sync_t *state, const char *group_id);
void effect_sync_cancel(effect_sync_t *state);
bool effect_sync_push(effect_sync_t *state, const effect_sync_frame_t *frame);
bool effect_sync_sample(effect_sync_t *state, uint32_t now_ms,
                        uint64_t utc_now_ms, effect_sync_frame_t *out);
