#include "pogmic.h"
#include "pogmic_pcm.h"

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <inttypes.h>

#ifdef CONFIG_POG_MICROPHONE
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#endif

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_active;
static uint32_t s_samples;
static float s_rms[2] = {-96.0f, -96.0f};
static float s_peak[2] = {-96.0f, -96.0f};
static uint32_t s_nonzero[2];
static esp_err_t s_error;
static bool s_clock_ready;
static bool s_stop;
static pogmic_stream_cb s_callback;
static void *s_callback_arg;

void pogmic_stream_stop(void) {
  portENTER_CRITICAL(&s_lock);
  s_stop = true;
  portEXIT_CRITICAL(&s_lock);
}

void pogmic_shared_clock_ready(void) {
  portENTER_CRITICAL(&s_lock);
  s_clock_ready = true;
  portEXIT_CRITICAL(&s_lock);
}

void pogmic_get_status(pogmic_status_t *status) {
  if (!status) {
    return;
  }
  *status = (pogmic_status_t){
      .bclk_gpio = -1, .ws_gpio = -1, .data_gpio = -1, .sample_rate = 16000};
#ifdef CONFIG_POG_MICROPHONE
  status->supported = true;
  status->bclk_gpio = CONFIG_POG_MIC_BCLK_GPIO;
  status->ws_gpio = CONFIG_POG_MIC_WS_GPIO;
  status->data_gpio = CONFIG_POG_MIC_DATA_GPIO;
#ifdef CONFIG_POG_MIC_RIGHT_SLOT
  status->right_slot = true;
#endif
  status->configured =
      status->bclk_gpio >= 0 && status->ws_gpio >= 0 && status->data_gpio >= 0;
#endif
#ifdef CONFIG_POG_MIC_SHARED_CLOCKS
  status->shared_clocks = true;
  status->sample_rate = CONFIG_OUTPUT_SAMPLE_RATE_HZ;
#endif
  portENTER_CRITICAL(&s_lock);
  status->clock_ready = !status->shared_clocks || s_clock_ready;
  status->active = s_active;
  status->samples = s_samples;
  status->rms_dbfs = s_rms[status->right_slot ? 1 : 0];
  status->peak_dbfs = s_peak[status->right_slot ? 1 : 0];
  for (size_t slot = 0; slot < 2; slot++) {
    status->slot_rms_dbfs[slot] = s_rms[slot];
    status->slot_peak_dbfs[slot] = s_peak[slot];
    status->slot_nonzero[slot] = s_nonzero[slot];
  }
  status->error = s_error;
  portEXIT_CRITICAL(&s_lock);
}

#ifdef CONFIG_POG_MICROPHONE
static void capture_task(void *arg) {
  const uint32_t duration_ms = (uint32_t)(uintptr_t)arg;
  pogmic_status_t status;
  pogmic_get_status(&status);
  portENTER_CRITICAL(&s_lock);
  pogmic_stream_cb callback = s_callback;
  void *callback_arg = s_callback_arg;
  portEXIT_CRITICAL(&s_lock);
  i2s_chan_handle_t rx = NULL;
  bool enabled = false;
  /* Keep task stack small; the I2S DMA buffers themselves stay in internal
   * DMA-capable RAM through the IDF driver. No recording buffer is allocated.
   */
  int32_t *raw = malloc(160 * 2 * sizeof(*raw));
  esp_err_t err = ESP_ERR_NO_MEM;
  if (!raw) {
    goto done;
  }
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(
      I2S_NUM_1, status.shared_clocks ? I2S_ROLE_SLAVE : I2S_ROLE_MASTER);
  channel.dma_desc_num = 4;
  channel.dma_frame_num = 160;
  err = i2s_new_channel(&channel, NULL, &rx);
  if (err != ESP_OK) {
    goto done;
  }
  i2s_std_config_t config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(status.sample_rate),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                   .bclk = status.bclk_gpio,
                   .ws = status.ws_gpio,
                   .dout = I2S_GPIO_UNUSED,
                   .din = status.data_gpio},
  };
  err = i2s_channel_init_std_mode(rx, &config);
  if (err != ESP_OK) {
    goto done;
  }
  err = i2s_channel_enable(rx);
  if (err != ESP_OK) {
    goto done;
  }
  enabled = true;
  int64_t deadline = esp_timer_get_time() + (int64_t)duration_ms * 1000;
  while (!duration_ms || esp_timer_get_time() < deadline) {
    portENTER_CRITICAL(&s_lock);
    bool stop = s_stop;
    portEXIT_CRITICAL(&s_lock);
    if (stop)
      break;
    size_t bytes = 0;
    /* IDF's channel API takes milliseconds, not FreeRTOS ticks. */
    err = i2s_channel_read(rx, raw, 160 * 2 * sizeof(*raw), &bytes, 100);
    if (err != ESP_OK) {
      break;
    }
    if (bytes == 0 || bytes % (2 * sizeof(*raw)) != 0) {
      err = ESP_ERR_INVALID_SIZE;
      break;
    }
    pogmic_levels_t levels = pogmic_pcm_measure(raw, bytes / sizeof(*raw));
    if (callback) {
      int16_t mono[160];
      size_t count = pogmic_pcm_decode(raw, bytes / sizeof(*raw),
                                       status.right_slot, mono, 160);
      err = callback(mono, count, status.sample_rate, ESP_OK, callback_arg);
      if (err != ESP_OK)
        break;
    }
    portENTER_CRITICAL(&s_lock);
    s_samples += levels.frames;
    for (size_t slot = 0; slot < 2; slot++) {
      s_rms[slot] = levels.dbfs[slot];
      s_nonzero[slot] += levels.nonzero[slot];
      if (levels.dbfs[slot] > s_peak[slot]) {
        s_peak[slot] = levels.dbfs[slot];
      }
    }
    portEXIT_CRITICAL(&s_lock);
  }
done:
  if (enabled) {
    i2s_channel_disable(rx);
  }
  if (rx) {
    i2s_del_channel(rx);
    /* Shared clock pins belong to TX, including on failure. The slave's
     * input routing must never disable or reset those speaker outputs. */
    if (!status.shared_clocks) {
      gpio_reset_pin(status.bclk_gpio);
      gpio_reset_pin(status.ws_gpio);
    }
    gpio_reset_pin(status.data_gpio);
  }
  free(raw);
  portENTER_CRITICAL(&s_lock);
  s_error = err;
  /* Copy the completed result before admitting a subsequent test. */
  uint32_t samples = s_samples;
  float left = s_peak[0], right = s_peak[1];
  uint32_t left_nonzero = s_nonzero[0], right_nonzero = s_nonzero[1];
  s_active = false;
  portEXIT_CRITICAL(&s_lock);
  if (callback)
    callback(NULL, 0, status.sample_rate, err, callback_arg);
  ESP_LOGI("pogmic",
           "test done: frames=%" PRIu32 " left_max=%.1f right_max=%.1f dBFS"
           " nonzero_left=%" PRIu32 " nonzero_right=%" PRIu32 " %s",
           samples, left, right, left_nonzero, right_nonzero,
           esp_err_to_name(err));
  vTaskDelete(NULL);
}
#endif

static esp_err_t capture_start(uint32_t duration_ms, uint64_t occupied,
                               pogmic_stream_cb cb, void *arg) {
#ifndef CONFIG_POG_MICROPHONE
  (void)duration_ms;
  (void)occupied;
  (void)cb;
  (void)arg;
  return ESP_ERR_NOT_SUPPORTED;
#else
  if ((!duration_ms && !cb) || (duration_ms && duration_ms < 1000) ||
      duration_ms > (cb ? 15000 : 10000)) {
    return ESP_ERR_INVALID_ARG;
  }
#ifdef CONFIG_IDF_TARGET_ESP32
  /* WROVER flash and PSRAM, serial console, and strapping pins. */
  occupied |= UINT64_C(0x3F) << 6;
  const int reserved[] = {0, 1, 2, 3, 5, 12, 15, 16, 17};
#else
  /* S3 flash/PSRAM (including octal pins), USB and strapping pins. */
  occupied |= UINT64_C(0x7FF) << 26;
  const int reserved[] = {0, 3, 19, 20, 45, 46};
#endif
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
    occupied |= UINT64_C(1) << reserved[i];
  }
  pogmic_status_t status;
  pogmic_get_status(&status);
  if (!status.clock_ready) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!pogmic_pins_valid(status.bclk_gpio, status.ws_gpio, status.data_gpio,
                         SOC_GPIO_VALID_GPIO_MASK,
                         SOC_GPIO_VALID_OUTPUT_GPIO_MASK, occupied)) {
    return ESP_ERR_INVALID_ARG;
  }
  portENTER_CRITICAL(&s_lock);
  if (s_active) {
    portEXIT_CRITICAL(&s_lock);
    return ESP_ERR_INVALID_STATE;
  }
  s_active = true;
  s_stop = false;
  s_callback = cb;
  s_callback_arg = arg;
  s_error = ESP_OK;
  s_samples = 0;
  for (size_t slot = 0; slot < 2; slot++) {
    s_rms[slot] = s_peak[slot] = -96.0f;
    s_nonzero[slot] = 0;
  }
  portEXIT_CRITICAL(&s_lock);
  /* Drain DMA ahead of the voice encoder; AirPlay decode occupies core 1. */
  if (xTaskCreatePinnedToCore(capture_task, "pogmic_test", 4096,
                              (void *)(uintptr_t)duration_ms, 6, NULL, 0) != pdPASS) {
    portENTER_CRITICAL(&s_lock);
    s_active = false;
    s_error = ESP_ERR_NO_MEM;
    portEXIT_CRITICAL(&s_lock);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
#endif
}

esp_err_t pogmic_test_start(uint32_t duration_ms, uint64_t occupied) {
  return capture_start(duration_ms, occupied, NULL, NULL);
}

esp_err_t pogmic_stream_start(uint64_t occupied, pogmic_stream_cb cb,
                              void *arg) {
  if (!cb)
    return ESP_ERR_INVALID_ARG;
  return capture_start(15000, occupied, cb, arg);
}

esp_err_t pogmic_monitor_start(uint64_t occupied, pogmic_stream_cb cb, void *arg) {
  if (!cb) return ESP_ERR_INVALID_ARG;
  return capture_start(0, occupied, cb, arg);
}
