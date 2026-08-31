#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Ignore le bruit numérique et les tampons pratiquement silencieux. À 96 sur
 * une pleine échelle int16, le seuil est voisin de -51 dBFS. */
#define LED_ARGB_SOUND_RMS_MIN 96.0f

/* Les petits trous entre deux tampons ne doivent pas faire clignoter la lampe
 * entre l'effet musical et sa couleur de repos. */
#define LED_ARGB_SOUND_HOLD_US 400000LL

bool led_argb_audio_is_audible(float rms);

/* Le mode musical appartient à POG Home : sans liaison active, ou sans son
 * récent, la bande reste une lampe classique. */
bool led_argb_music_should_override(bool home_connected, bool music_enabled,
                                    bool sound_seen, int64_t last_sound_us,
                                    int64_t now_us);

/* Les effets ambiants (arc-en-ciel, veilleuse, couleur, respiration, comète,
 * scintillement) n'ont pas besoin d'un morceau audible. Les effets réactifs,
 * eux, retombent sur la lampe statique lorsque le flux musical s'arrête. */
bool led_argb_effect_should_render(bool home_connected, bool effects_enabled,
                                   bool audio_reactive, bool sound_seen,
                                   int64_t last_sound_us, int64_t now_us);
