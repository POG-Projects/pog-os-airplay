#include "led_argb_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
  assert(!led_argb_audio_is_audible(95.9f));
  assert(led_argb_audio_is_audible(96.0f));

  const int64_t last_sound = 1000000;
  assert(!led_argb_music_should_override(false, true, true, last_sound,
                                         last_sound));
  assert(!led_argb_music_should_override(true, false, true, last_sound,
                                         last_sound));
  assert(!led_argb_music_should_override(true, true, false, last_sound,
                                         last_sound));
  assert(led_argb_music_should_override(true, true, true, last_sound,
                                        last_sound + LED_ARGB_SOUND_HOLD_US));
  assert(!led_argb_music_should_override(
      true, true, true, last_sound, last_sound + LED_ARGB_SOUND_HOLD_US + 1));
  assert(!led_argb_music_should_override(true, true, true, last_sound,
                                         last_sound - 1));

  assert(
      led_argb_effect_should_render(true, true, false, false, 0, last_sound));
  assert(
      !led_argb_effect_should_render(false, true, false, false, 0, last_sound));
  assert(
      !led_argb_effect_should_render(true, false, false, false, 0, last_sound));
  assert(
      !led_argb_effect_should_render(true, true, true, false, 0, last_sound));
  assert(led_argb_effect_should_render(true, true, true, true, last_sound,
                                       last_sound));

  puts("led_argb_policy: OK");
  return 0;
}
