#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
  bool supported;
  bool configured;
  bool active;
  bool right_slot;
  bool shared_clocks;
  bool clock_ready;
  int bclk_gpio;
  int ws_gpio;
  int data_gpio;
  uint32_t sample_rate;
  uint32_t samples;
  float rms_dbfs;
  float peak_dbfs;
  float slot_rms_dbfs[2];
  float slot_peak_dbfs[2];
  uint32_t slot_nonzero[2];
  esp_err_t error;
} pogmic_status_t;

/* Read-only metadata. Does not initialize I2S or start listening. */
void pogmic_get_status(pogmic_status_t *status);

/* Speaker integration: call only after the fixed-rate I2S0 TX is enabled. */
void pogmic_shared_clock_ready(void);

/* Explicit local level test, 1..10 seconds. Does not retain or send audio.
 * occupied contains pins used by the application (including runtime options).
 * Only one test can run; GPIOs and DMA are released on completion or failure.
 * Successful return means scheduled; inspect status.error for hardware errors.
 */
esp_err_t pogmic_test_start(uint32_t duration_ms, uint64_t occupied);

/* Explicit bounded stream, exclusive with diagnostics. Callback must not block.
 * Final callback has pcm=NULL and reports cleanup completion/error. */
typedef esp_err_t (*pogmic_stream_cb)(const int16_t *pcm, size_t frames,
                                      uint32_t rate, esp_err_t error,
                                      void *arg);
esp_err_t pogmic_stream_start(uint64_t occupied, pogmic_stream_cb cb,
                              void *arg);
void pogmic_stream_stop(void);
/* Local detector only; runs until stop. Does not transmit or retain audio. */
esp_err_t pogmic_monitor_start(uint64_t occupied, pogmic_stream_cb cb,
                               void *arg);
