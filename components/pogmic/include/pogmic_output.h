#pragma once

#include "driver/i2s_std.h"

/* PCM16 stereo -> PCM32 stereo for a shared 64-BCLK I2S frame.
 * Like the speaker backend, this expects a single writer at a time.
 * timeout_ms and written refer to the complete input buffer. */
esp_err_t pogmic_output_write(i2s_chan_handle_t tx, const void *pcm,
                              size_t bytes, size_t *written,
                              uint32_t timeout_ms);
