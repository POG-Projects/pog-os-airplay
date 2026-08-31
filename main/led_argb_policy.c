#include "led_argb_policy.h"

bool led_argb_audio_is_audible(float rms) {
  return rms >= LED_ARGB_SOUND_RMS_MIN;
}

bool led_argb_music_should_override(bool home_connected, bool music_enabled,
                                    bool sound_seen, int64_t last_sound_us,
                                    int64_t now_us) {
  if (!home_connected || !music_enabled || !sound_seen ||
      now_us < last_sound_us) {
    return false;
  }
  return now_us - last_sound_us <= LED_ARGB_SOUND_HOLD_US;
}

bool led_argb_effect_should_render(bool home_connected, bool effects_enabled,
                                   bool audio_reactive, bool sound_seen,
                                   int64_t last_sound_us, int64_t now_us) {
  if (!home_connected || !effects_enabled) {
    return false;
  }
  if (!audio_reactive) {
    return true;
  }
  return led_argb_music_should_override(home_connected, effects_enabled,
                                        sound_seen, last_sound_us, now_us);
}
