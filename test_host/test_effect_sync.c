#include "../main/effect_sync.h"

#include <assert.h>
#include <math.h>

int main(void) {
  const char *group = "01234567-89ab-cdef-0123-456789abcdef";
  const char *leader = "fedcba98-7654-3210-fedc-ba9876543210";
  effect_sync_t sync = {0};
  assert(!effect_sync_join(&sync, group, "leader", leader, 120, 0, NULL));
  assert(effect_sync_join(&sync, group, "follower", leader, 120, 0, NULL));
  assert(sync.visualizer == EFFECT_VISUALIZER_SPECTRUM);
  assert(!effect_sync_join(&sync, group, "follower", leader, 0, 0,
                           "unknown"));
  assert(effect_sync_join(&sync, group, "follower", leader, 0, 0,
                          "bass_pulse"));
  assert(sync.visualizer == EFFECT_VISUALIZER_BASS_PULSE);
  assert(effect_sync_join(&sync, group, "follower", leader, 120, 0, NULL));
  effect_sync_frame_t b = {2, 1040, 0, 40, .6f, .8f, 1.f, 140};
  effect_sync_frame_t a = {1, 1000, 0, 40, .2f, .4f, .6f, 100};
  assert(effect_sync_push(&sync, &b));
  assert(effect_sync_push(&sync, &a));
  effect_sync_frame_t out;
  assert(effect_sync_sample(&sync, 160, 0, &out));
  assert(fabsf(out.level - .4f) < .001f);
  assert(!effect_sync_push(&sync, &a));
  assert(!effect_sync_sample(&sync, 641, 0, &out));
  assert(effect_sync_leave(&sync, group));
  assert(effect_sync_join(&sync, group, "follower", leader, 0, 10, NULL));
  effect_sync_frame_t utc = {3, 1080, 1760000000100ULL, 0,
                             .5f, .5f, .5f, 200};
  assert(effect_sync_push(&sync, &utc));
  assert(!effect_sync_sample(&sync, 210, 1760000000109ULL, &out));
  assert(effect_sync_sample(&sync, 211, 1760000000110ULL, &out));
  assert(fabsf(effect_sync_pixel_position(0, 4) - .125f) < .001f);
  assert(fabsf(effect_sync_pixel_position(3, 4) - .875f) < .001f);
  assert(fabsf(effect_sync_pixel_position(299, 300) -
               (299.5f / 300.0f)) < .001f);

  out.mono_ms = 2000;
  out.level = .5f;
  out.bass = .25f;
  out.treble = .75f;
  effect_sync_pixel_t pixel = effect_sync_visualizer_pixel(
      EFFECT_VISUALIZER_SPECTRUM, .5f, &out);
  assert(fabsf(pixel.hue - (1.0f / 3.0f)) < .001f);
  assert(fabsf(pixel.value - .5f) < .001f);
  assert(effect_sync_visualizer_pixel(EFFECT_VISUALIZER_VU_METER, .25f,
                                      &out).value == 1.0f);
  assert(effect_sync_visualizer_pixel(EFFECT_VISUALIZER_VU_METER, .75f,
                                      &out).value == 0.0f);
  effect_sync_pixel_t left = effect_sync_visualizer_pixel(
      EFFECT_VISUALIZER_BASS_PULSE, .1f, &out);
  effect_sync_pixel_t right = effect_sync_visualizer_pixel(
      EFFECT_VISUALIZER_BASS_PULSE, .9f, &out);
  assert(left.hue == right.hue && left.value == right.value);
  pixel = effect_sync_visualizer_pixel(EFFECT_VISUALIZER_RAINBOW, .5f, &out);
  assert(fabsf(pixel.hue - .75f) < .001f);
  assert(fabsf(pixel.value - .5f) < .001f);
}
