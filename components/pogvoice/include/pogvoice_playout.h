#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Called under the speaker queue lock. Keep partial network arrivals in the
 * queue until a complete output block is available; only the final tail may
 * be padded with silence. Rebuffer after a real underrun. */
typedef struct {
  bool primed, finished;
  uint32_t underruns, played_samples;
} pogvoice_playout_t;

static inline size_t pogvoice_playout_take(pogvoice_playout_t *p,
                                           size_t available, size_t requested,
                                           size_t prebuffer) {
  if (!p->primed && (available >= prebuffer || p->finished))
    p->primed = true;
  if (!p->primed)
    return 0;
  if (available < requested && !p->finished) {
    p->primed = false;
    p->underruns++;
    return 0;
  }
  size_t count = available < requested ? available : requested;
  p->played_samples += count;
  return count;
}
