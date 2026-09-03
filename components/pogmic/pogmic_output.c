#include "pogmic_output.h"
#include "pogmic_pcm.h"
#include "esp_timer.h"

esp_err_t pogmic_output_write(i2s_chan_handle_t tx, const void *pcm,
                              size_t bytes, size_t *written,
                              uint32_t timeout_ms) {
  if (!pcm || !written || bytes % (2 * sizeof(int16_t)) != 0) {
    return ESP_ERR_INVALID_ARG;
  }
  *written = 0;
  const int16_t *input = pcm;
  /* Static scratch avoids adding 1 KiB to the playback task's small stack.
   * AirPlay and Bluetooth already serialize access to the output backend. */
  static int32_t wide[128 * 2];
  const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
  while (*written < bytes) {
    size_t count = (bytes - *written) / sizeof(*input);
    if (count > 128 * 2) {
      count = 128 * 2;
    }
    pogmic_pcm_expand(input + *written / sizeof(*input), wide, count);
    int64_t remaining = deadline - esp_timer_get_time();
    uint32_t wait = remaining > 0 ? (uint32_t)((remaining + 999) / 1000) : 0;
    size_t sent = 0;
    esp_err_t err =
        i2s_channel_write(tx, wide, count * sizeof(*wide), &sent, wait);
    *written += sent / 2;
    if (err != ESP_OK) {
      return err;
    }
    if (sent != count * sizeof(*wide)) {
      return ESP_ERR_INVALID_SIZE;
    }
  }
  return ESP_OK;
}
