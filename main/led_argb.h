#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t feed_calls;
  float level;
  float bass;
  float treble;
} led_argb_audio_stats_t;

/**
 * Optional addressable RGB LED strip (WS2812 / WS2812B / SK6812),
 * audio-reactive. Driven over the RMT peripheral. Replaces the MAX7219 matrix
 * as the headline "light effects" feature, but is independent of it.
 *
 * DISABLED by default. When disabled it allocates no RMT channel, runs no task,
 * and led_argb_feed() returns immediately — zero cost for devices that do not
 * opt in.
 *
 * User-configurable: data GPIO, LED count, brightness, colour and musical
 * effect. While linked to POG Home, the strip behaves as a regular lamp and
 * the musical effect overrides it only when explicitly enabled and audible
 * audio is present. Effects
 * (argb_fx): 0 = VU-metre, 1 = Spectre, 2 = Pulsation basses, 3 = Arc-en-ciel
 * (ambient), 4 = Veilleuse (ambient).
 */

/**
 * Initialize the strip subsystem (call once at startup, after settings_init()).
 * Reads NVS config; if disabled, does nothing.
 */
void led_argb_init(void);

/**
 * Apply a config change at runtime. Safe to call repeatedly from the web
 * handler. Stops/frees the RMT channel if now disabled or if the GPIO/count
 * changed, then (re)starts the renderer if enabled.
 */
void led_argb_reconfigure(void);

/**
 * Lightweight audio analysis tap. Call from the audio playback task next to
 * led_audio_feed()/led_matrix_feed(). Returns immediately (cheap) when
 * disabled.
 *
 * @param pcm            Interleaved stereo int16 samples (L,R,L,R,...)
 * @param stereo_samples Number of stereo frames (pcm holds 2*stereo_samples
 *                       int16 values).
 */
void led_argb_feed(const int16_t *pcm, size_t stereo_samples);

/** Snapshot the audio values currently consumed by reactive effects. */
void led_argb_get_audio_stats(led_argb_audio_stats_t *stats);

bool led_argb_effect_sync_join(const char *group_id,
                               const char *leader_entity_id,
                               int presentation_delay_ms,
                               int calibration_offset_ms,
                               const char *visualizer);
bool led_argb_effect_sync_leave(const char *group_id);
void led_argb_effect_sync_cancel(void);
bool led_argb_effect_sync_frame(uint32_t seq, uint64_t mono_ms,
                                uint64_t present_at_ms, uint16_t lead_ms,
                                float level, float bass, float treble,
                                uint32_t received_ms);
