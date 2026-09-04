#include "effect_sync.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

static bool seq_after(uint32_t candidate, uint32_t reference) {
  return (int32_t)(candidate - reference) > 0;
}

bool effect_sync_uuid_is_canonical(const char *value) {
  if (value == NULL || strlen(value) != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
    } else if (!isxdigit((unsigned char)value[i])) {
      return false;
    }
  }
  return true;
}

bool effect_sync_visualizer_from_name(const char *value,
                                      effect_visualizer_t *out) {
  if (out == NULL) return false;
  if (value == NULL || strcmp(value, "spectrum") == 0) {
    *out = EFFECT_VISUALIZER_SPECTRUM;
    return true;
  }
  if (strcmp(value, "vu_meter") == 0) {
    *out = EFFECT_VISUALIZER_VU_METER;
    return true;
  }
  if (strcmp(value, "bass_pulse") == 0) {
    *out = EFFECT_VISUALIZER_BASS_PULSE;
    return true;
  }
  if (strcmp(value, "rainbow") == 0) {
    *out = EFFECT_VISUALIZER_RAINBOW;
    return true;
  }
  return false;
}

float effect_sync_pixel_position(size_t index, size_t count) {
  return count ? ((float)index + 0.5f) / (float)count : 0.0f;
}

effect_sync_pixel_t effect_sync_visualizer_pixel(
    effect_visualizer_t visualizer, float position,
    const effect_sync_frame_t *frame) {
  effect_sync_pixel_t pixel = {0, 1, 0};
  if (frame == NULL) return pixel;
  if (position < 0) position = 0;
  if (position > 1) position = 1;
  switch (visualizer) {
    case EFFECT_VISUALIZER_SPECTRUM:
      pixel.hue = position * (2.0f / 3.0f);
      pixel.value = frame->bass + (frame->treble - frame->bass) * position;
      break;
    case EFFECT_VISUALIZER_VU_METER:
      pixel.hue = (1.0f - position) / 3.0f;
      pixel.value = position <= frame->level ? 1.0f : 0.0f;
      break;
    case EFFECT_VISUALIZER_BASS_PULSE:
      pixel.hue = 1.0f / 12.0f;
      pixel.value = frame->bass;
      break;
    case EFFECT_VISUALIZER_RAINBOW:
      pixel.hue = position + (float)(frame->mono_ms % 8000ULL) / 8000.0f;
      if (pixel.hue >= 1.0f) pixel.hue -= 1.0f;
      pixel.value = frame->level;
      break;
  }
  return pixel;
}

bool effect_sync_join(effect_sync_t *state, const char *group_id,
                      const char *role, const char *leader_entity_id,
                      int presentation_delay_ms, int calibration_offset_ms,
                      const char *visualizer) {
  effect_visualizer_t parsed_visualizer;
  if (state == NULL || !effect_sync_uuid_is_canonical(group_id) ||
      !effect_sync_uuid_is_canonical(leader_entity_id) || role == NULL ||
      strcmp(role, "follower") != 0 || presentation_delay_ms < 0 ||
      presentation_delay_ms > 500 || calibration_offset_ms < -100 ||
      calibration_offset_ms > 100 ||
      !effect_sync_visualizer_from_name(visualizer, &parsed_visualizer))
    return false;
  memset(state, 0, sizeof(*state));
  memcpy(state->group_id, group_id, 37);
  memcpy(state->leader_entity_id, leader_entity_id, 37);
  state->presentation_delay_ms = (uint16_t)presentation_delay_ms;
  state->calibration_offset_ms = (int16_t)calibration_offset_ms;
  state->visualizer = parsed_visualizer;
  state->joined = true;
  return true;
}

bool effect_sync_leave(effect_sync_t *state, const char *group_id) {
  if (state == NULL || !state->joined ||
      !effect_sync_uuid_is_canonical(group_id) ||
      strcmp(state->group_id, group_id) != 0) return false;
  memset(state, 0, sizeof(*state));
  return true;
}

void effect_sync_cancel(effect_sync_t *state) {
  if (state != NULL) memset(state, 0, sizeof(*state));
}

bool effect_sync_push(effect_sync_t *state, const effect_sync_frame_t *frame) {
  if (state == NULL || frame == NULL || !state->joined ||
      !isfinite(frame->level) || !isfinite(frame->bass) ||
      !isfinite(frame->treble) || frame->level < 0 || frame->level > 1 ||
      frame->bass < 0 || frame->bass > 1 || frame->treble < 0 ||
      frame->treble > 1) return false;
  if (state->has_rendered &&
      !seq_after(frame->seq, state->last_rendered_seq)) return false;
  size_t at = 0;
  while (at < state->frame_count &&
         seq_after(frame->seq, state->frames[at].seq)) ++at;
  if (at < state->frame_count && state->frames[at].seq == frame->seq)
    return false;
  if (state->frame_count == EFFECT_SYNC_CAPACITY) {
    if (at == 0) return false;
    memmove(state->frames, state->frames + 1,
            (EFFECT_SYNC_CAPACITY - 1) * sizeof(*frame));
    --state->frame_count;
    --at;
  }
  memmove(state->frames + at + 1, state->frames + at,
          (state->frame_count - at) * sizeof(*frame));
  state->frames[at] = *frame;
  ++state->frame_count;
  return true;
}

bool effect_sync_sample(effect_sync_t *state, uint32_t now_ms,
                        uint64_t utc_now_ms, effect_sync_frame_t *out) {
  if (state == NULL || out == NULL || !state->joined || !state->frame_count ||
      now_ms - state->frames[state->frame_count - 1].received_ms >
          EFFECT_SYNC_TIMEOUT_MS) return false;
  bool utc = utc_now_ms > 1700000000000ULL && state->frames[0].present_at_ms;
  int64_t target = (utc ? (int64_t)utc_now_ms : (int64_t)now_ms) -
                   state->calibration_offset_ms;
#define FRAME_TIME(frame) \
  (utc ? (int64_t)(frame).present_at_ms \
       : (int64_t)(frame).received_ms + (frame).lead_ms)
  if (target < FRAME_TIME(state->frames[0])) return false;
  size_t before = 0;
  while (before + 1 < state->frame_count &&
         FRAME_TIME(state->frames[before + 1]) <= target) ++before;
  *out = state->frames[before];
  if (before + 1 < state->frame_count) {
    int64_t start = FRAME_TIME(state->frames[before]);
    int64_t end = FRAME_TIME(state->frames[before + 1]);
    if (end > start) {
      float mix = (float)(target - start) / (float)(end - start);
      out->level += (state->frames[before + 1].level - out->level) * mix;
      out->bass += (state->frames[before + 1].bass - out->bass) * mix;
      out->treble += (state->frames[before + 1].treble - out->treble) * mix;
    }
  }
  state->has_rendered = true;
  state->last_rendered_seq = state->frames[before].seq;
  if (before > 0) {
    memmove(state->frames, state->frames + before,
            (state->frame_count - before) * sizeof(*out));
    state->frame_count -= before;
  }
#undef FRAME_TIME
  return true;
}
