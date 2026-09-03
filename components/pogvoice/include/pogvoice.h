#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include "pogmic.h"
#include "pogvoice_feedback.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef esp_err_t (*pogvoice_capture_start_fn)(pogmic_stream_cb, void *);
void pogvoice_init(pogvoice_capture_start_fn capture);
cJSON *pogvoice_status(void);
esp_err_t pogvoice_enroll(const char *api_url, const char *code,
                          bool allow_lan);
esp_err_t pogvoice_forget(void);
esp_err_t pogvoice_start(void);
esp_err_t pogvoice_start_wake(const char *phrase);
void pogvoice_finish(void);
void pogvoice_cancel(void);
bool pogvoice_busy(void);
bool pogvoice_paired(void);
pogvoice_light_t pogvoice_light_state(void);
/* True means music is suppressed. PCM is stereo at the speaker rate. */
bool pogvoice_speaker_read(int16_t *stereo, size_t capacity, size_t *frames);
