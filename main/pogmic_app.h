#pragma once
#include "pogmic.h"

esp_err_t pogmic_app_stream_start(pogmic_stream_cb cb, void *arg);
esp_err_t pogmic_app_monitor_start(pogmic_stream_cb cb, void *arg);
/* Authenticated training/sample capture. The caller owns storage and must
 * stop the stream once its explicit bounded sample is complete. */
esp_err_t pogmic_app_sample_start(pogmic_stream_cb cb, void *arg);

#include "cJSON.h"
#include "esp_err.h"

cJSON *pogmic_app_status(void);
esp_err_t pogmic_app_test_start(void);
