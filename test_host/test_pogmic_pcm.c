#include "pogmic_pcm.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
  const int32_t raw[] = {INT32_MAX, INT32_MIN, 0x12345600, -65536, 0};
  int16_t mono[3] = {0, 0, 123};
  assert(pogmic_pcm_decode(raw, 5, false, mono, 3) == 2);
  assert(mono[0] == 32767 && mono[1] == 0x1234 && mono[2] == 123);
  assert(pogmic_pcm_decode(raw, 4, true, mono, 3) == 2);
  assert(mono[0] == -32768 && mono[1] == -1);
  assert(pogmic_pcm_decode(raw, 4, false, mono, 1) == 1);
  assert(pogmic_pcm_decode(NULL, 4, false, mono, 3) == 0);
  assert(pogmic_pcm_decode(raw, 1, false, mono, 3) == 0);
  const int16_t silence[] = {0, 0};
  const int16_t half[] = {16384, -16384};
  assert(pogmic_pcm_dbfs(silence, 2) == -96.0f);
  assert(pogmic_pcm_dbfs(NULL, 0) == -96.0f);
  assert(fabsf(pogmic_pcm_dbfs(half, 2) + 6.0206f) < 0.001f);

  /* A microphone on the unselected slot must remain visible in diagnostics. */
  const int32_t right_only[] = {0, 1073741824, 0, -1073741824, INT32_MAX};
  pogmic_levels_t levels = pogmic_pcm_measure(right_only, 5);
  assert(levels.frames == 2 && levels.nonzero[0] == 0 &&
         levels.nonzero[1] == 2);
  assert(levels.dbfs[0] == -96.0f);
  assert(fabsf(levels.dbfs[1] + 6.0206f) < 0.001f);
  const int32_t left_only[] = {INT32_MIN, 0, INT32_MAX, 0};
  levels = pogmic_pcm_measure(left_only, 4);
  assert(levels.nonzero[0] == 2 && levels.nonzero[1] == 0);
  assert(fabsf(levels.dbfs[0]) < 0.001f && levels.dbfs[1] == -96.0f);
  const int32_t tiny[] = {256, 0, -256, 0};
  levels = pogmic_pcm_measure(tiny, 4);
  assert(levels.dbfs[0] == -96.0f && levels.nonzero[0] == 2);
  const int32_t zeros[] = {0, 0, 0, 0};
  levels = pogmic_pcm_measure(zeros, 4);
  assert(levels.nonzero[0] == 0 && levels.nonzero[1] == 0);
  assert(levels.dbfs[0] == -96.0f && levels.dbfs[1] == -96.0f);
  assert(pogmic_pcm_measure(NULL, 4).frames == 0);
  assert(pogmic_pcm_measure(right_only, 1).frames == 0);

  const int16_t pcm[] = {-32768, 32767, -1, 0, 1, 16384};
  int32_t wide[6];
  pogmic_pcm_expand(pcm, wide, 6);
  assert(wide[0] == INT32_MIN && wide[1] == 2147418112);
  assert(wide[2] == -65536 && wide[3] == 0);
  assert(wide[4] == 65536 && wide[5] == 1073741824);
  assert(pogmic_pcm_decode(wide, 6, false, mono, 3) == 3);
  assert(mono[0] == pcm[0] && mono[1] == pcm[2] && mono[2] == pcm[4]);
  assert(pogmic_pcm_decode(wide, 6, true, mono, 3) == 3);
  assert(mono[0] == pcm[1] && mono[1] == pcm[3] && mono[2] == pcm[5]);

  uint64_t inputs = (UINT64_C(1) << 40) - 1;
  uint64_t outputs = (UINT64_C(1) << 34) - 1;
  assert(pogmic_shared_pins_valid(32, 33, 35, 32, 33, 25, outputs));
  assert(!pogmic_shared_pins_valid(32, 33, 35, 32, 33, 34, outputs));
  assert(!pogmic_shared_pins_valid(32, 33, 35, 33, 32, 25, outputs));
  assert(!pogmic_shared_pins_valid(32, 33, 35, 32, 33, 35, outputs));
  assert(!pogmic_shared_pins_valid(32, 33, 35, 32, 33, 32, outputs));
  assert(!pogmic_shared_pins_valid(32, 33, 35, 32, 33, -1, outputs));
  assert(!pogmic_shared_pins_valid(32, 33, 35, 32, 33, 64, outputs));
  /* Shared clocks may be excluded from the speaker reservation only;
   * another peripheral owning either clock must still block capture. */
  assert(pogmic_pins_valid(32, 33, 35, inputs, outputs, UINT64_C(1) << 25));
  assert(!pogmic_pins_valid(32, 33, 35, inputs, outputs, UINT64_C(1) << 32));
  uint64_t occupied = (UINT64_C(0x3F) << 6) | (UINT64_C(3) << 16) |
                      (UINT64_C(1) << 33) | (UINT64_C(1) << 25) |
                      (UINT64_C(1) << 32);
  assert(pogmic_pins_valid(26, 27, 34, inputs, outputs, occupied));
  assert(!pogmic_pins_valid(34, 27, 26, inputs, outputs, occupied));
  assert(!pogmic_pins_valid(26, 26, 34, inputs, outputs, occupied));
  assert(!pogmic_pins_valid(-1, 27, 34, inputs, outputs, occupied));
  assert(!pogmic_pins_valid(26, 27, 64, inputs, outputs, occupied));
  assert(!pogmic_pins_valid(33, 27, 34, inputs, outputs, occupied));
  assert(!pogmic_pins_valid(26, 27, 16, inputs, outputs, occupied));
  assert(!pogmic_pins_valid(26, 27, 6, inputs, outputs, occupied));
  assert(!pogmic_pins_valid(26, 27, 40, inputs, outputs, occupied));
  puts("pogmic PCM and GPIO safety: OK");
}
