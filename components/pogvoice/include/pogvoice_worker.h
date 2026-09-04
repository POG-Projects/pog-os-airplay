#pragma once
#include "esp_err.h"

/* One internal stack, shared sequentially by detection, pairing and voice.
 * Jobs must return normally; they must never delete or suspend this task. */
#define POGVOICE_WORKER_STACK_BYTES (40 * 1024)
typedef void (*pogvoice_audio_job_fn)(void *);

/* Called once at boot, before Wi-Fi and wake inference fragment internal RAM. */
esp_err_t pogvoice_worker_init(void);
/* One pending job maximum. Nonblocking; callers keep their lifecycle gates. */
esp_err_t pogvoice_worker_submit(pogvoice_audio_job_fn run, void *arg,
                                unsigned priority);
