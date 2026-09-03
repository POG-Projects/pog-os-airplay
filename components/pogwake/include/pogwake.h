#pragma once
#include "pogmic.h"
#include <stdbool.h>

typedef struct {
  bool supported, enabled, active;
  unsigned model, detections, overruns;
  char phrase[24];
  esp_err_t error;
} pogwake_status_t;

void pogwake_init(esp_err_t (*capture)(pogmic_stream_cb, void *),
                  bool (*available)(void), esp_err_t (*trigger)(const char *));
esp_err_t pogwake_configure(bool enabled, unsigned model);
/* Temporary pause for a manual conversation. Does not change preference. */
void pogwake_pause(void);
/* Hold the microphone off throughout OTA, without changing the saved mode. */
void pogwake_suspend(bool suspended);
void pogwake_get_status(pogwake_status_t *status);
