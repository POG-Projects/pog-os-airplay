#include "pogmic_app.h"

#include "pogmic.h"
#include "pogmic_pcm.h"
#include "pogvoice.h"
#include "sdkconfig.h"
#include "settings.h"
#include "esp_bit_defs.h"
#include "soc/soc_caps.h"

cJSON *pogmic_app_status(void) {
  pogmic_status_t status;
  pogmic_get_status(&status);
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    return NULL;
  }
  cJSON_AddBoolToObject(json, "supported", status.supported);
  cJSON_AddBoolToObject(json, "configured", status.configured);
  cJSON_AddBoolToObject(json, "active", status.active);
  cJSON_AddBoolToObject(json, "shared_clocks", status.shared_clocks);
  cJSON_AddBoolToObject(json, "clock_ready", status.clock_ready);
  cJSON_AddStringToObject(json, "slot", status.right_slot ? "right" : "left");
  cJSON_AddNumberToObject(json, "bclk_gpio", status.bclk_gpio);
  cJSON_AddNumberToObject(json, "ws_gpio", status.ws_gpio);
  cJSON_AddNumberToObject(json, "data_gpio", status.data_gpio);
  cJSON_AddNumberToObject(json, "sample_rate", status.sample_rate);
  cJSON_AddNumberToObject(json, "samples", status.samples);
  cJSON_AddNumberToObject(json, "rms_dbfs", status.rms_dbfs);
  cJSON_AddNumberToObject(json, "peak_rms_dbfs", status.peak_dbfs);
  cJSON_AddNumberToObject(json, "left_rms_dbfs", status.slot_rms_dbfs[0]);
  cJSON_AddNumberToObject(json, "right_rms_dbfs", status.slot_rms_dbfs[1]);
  cJSON_AddNumberToObject(json, "left_peak_rms_dbfs", status.slot_peak_dbfs[0]);
  cJSON_AddNumberToObject(json, "right_peak_rms_dbfs",
                          status.slot_peak_dbfs[1]);
  cJSON_AddNumberToObject(json, "left_nonzero_words", status.slot_nonzero[0]);
  cJSON_AddNumberToObject(json, "right_nonzero_words", status.slot_nonzero[1]);
  cJSON_AddStringToObject(json, "error", esp_err_to_name(status.error));
  /* Configured for on-demand voice; the live connection state is in voice. */
  cJSON_AddBoolToObject(json, "satellite_ready",
                        pogvoice_paired() && status.configured &&
                            status.clock_ready);
  return json;
}

static void reserve_pin(uint64_t *occupied, int pin) {
  if (pin >= 0 && pin < 64) {
    *occupied |= UINT64_C(1) << pin;
  }
}

static esp_err_t start_capture(pogmic_stream_cb cb, void *arg, bool monitor) {
  /* Other boards need their own GPIO/peripheral ownership audit before
   * driving a clock; this first integration targets generic I2S boards. */
#if defined(CONFIG_DISPLAY_ENABLED) || defined(CONFIG_ETH_W5500_ENABLED) || \
    defined(CONFIG_AUDIO_OUTPUT_SPDIF) || defined(CONFIG_AUDIO_OUTPUT_USB)
  return ESP_ERR_NOT_SUPPORTED;
#endif
  uint64_t occupied = 0;
#ifdef CONFIG_POG_MIC_SHARED_CLOCKS
  /* Only this exact pair of speaker clocks can be shared. Every other
   * peripheral reservation below still applies, including runtime settings. */
  if (!pogmic_shared_pins_valid(
          CONFIG_POG_MIC_BCLK_GPIO, CONFIG_POG_MIC_WS_GPIO,
          CONFIG_POG_MIC_DATA_GPIO, CONFIG_I2S_BCK_IO, CONFIG_I2S_WS_IO,
          CONFIG_I2S_DO_IO, SOC_GPIO_VALID_OUTPUT_GPIO_MASK)) {
    return ESP_ERR_INVALID_ARG;
  }
#else
  reserve_pin(&occupied, CONFIG_I2S_BCK_IO);
  reserve_pin(&occupied, CONFIG_I2S_WS_IO);
#endif
  const int fixed[] = {
      CONFIG_I2S_DO_IO,       CONFIG_I2S_SCK_IO,     CONFIG_I2S_GND_IO,
      CONFIG_I2S_VCC_IO,      CONFIG_DAC_I2C_SDA,    CONFIG_DAC_I2C_SCL,
      CONFIG_LED_STATUS_GPIO, CONFIG_LED_ERROR_GPIO, CONFIG_LED_RGB_GPIO,
      CONFIG_MUTE_GPIO,       CONFIG_JACK_GPIO,      CONFIG_SPKFAULT_GPIO,
      CONFIG_BAT_CHANNEL};
  for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
    reserve_pin(&occupied, fixed[i]);
  }
  int buttons[5];
  settings_get_buttons(&buttons[0], &buttons[1], &buttons[2], &buttons[3],
                       &buttons[4]);
  for (size_t i = 0; i < 5; i++) {
    reserve_pin(&occupied, buttons[i]);
  }
  bool enabled = false;
  int pin = -1;
  settings_get_argb(&enabled, &pin, NULL, NULL, NULL, NULL, NULL, NULL);
  if (enabled) {
    reserve_pin(&occupied, pin);
  }
  int din = -1, clk = -1, cs = -1;
  settings_get_matrix(&enabled, NULL, NULL, &din, &clk, &cs);
  if (enabled) {
    reserve_pin(&occupied, din);
    reserve_pin(&occupied, clk);
    reserve_pin(&occupied, cs);
  }
  settings_get_protection(NULL, NULL, &pin, NULL, NULL);
  reserve_pin(&occupied, pin);
  return cb ? (monitor ? pogmic_monitor_start(occupied, cb, arg)
                       : pogmic_stream_start(occupied, cb, arg))
            : pogmic_test_start(5000, occupied);
}

esp_err_t pogmic_app_test_start(void) {
  if (pogvoice_busy())
    return ESP_ERR_INVALID_STATE;
  return start_capture(NULL, NULL, false);
}

esp_err_t pogmic_app_stream_start(pogmic_stream_cb cb, void *arg) {
  if (!cb)
    return ESP_ERR_INVALID_ARG;
  return start_capture(cb, arg, false);
}

esp_err_t pogmic_app_monitor_start(pogmic_stream_cb cb, void *arg) {
  if (!cb)
    return ESP_ERR_INVALID_ARG;
  return start_capture(cb, arg, true);
}

esp_err_t pogmic_app_sample_start(pogmic_stream_cb cb, void *arg) {
  if (!cb)
    return ESP_ERR_INVALID_ARG;
  if (pogvoice_busy())
    return ESP_ERR_INVALID_STATE;
  return start_capture(cb, arg, false);
}
