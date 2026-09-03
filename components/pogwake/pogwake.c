#include "pogwake.h"
#include "sdkconfig.h"
#include <string.h>

#ifndef CONFIG_POG_WAKEWORD
void pogwake_init(esp_err_t (*c)(pogmic_stream_cb, void *), bool (*a)(void), esp_err_t (*t)(const char *)) {
  (void)c; (void)a; (void)t;
}
esp_err_t pogwake_configure(bool enabled, unsigned model) {
  (void)enabled; (void)model;
  return ESP_ERR_NOT_SUPPORTED;
}
void pogwake_pause(void) {}
void pogwake_suspend(bool suspended) { (void)suspended; }
void pogwake_get_status(pogwake_status_t *s) { memset(s, 0, sizeof(*s)); }
#else
#include "pogwake_engine.h"
#include "pogvoice_resample.h"
#include "pogvoice_worker.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <stdatomic.h>

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static pogwake_status_t s_status;
static uint32_t s_generation;
static uint32_t s_worker_generation;
static bool s_pending, s_suspended;
/* Serialize starting a turn with mute/OTA. Never hold a critical section
 * while allocating memory or calling the voice component. */
static StaticSemaphore_t s_control_storage;
static SemaphoreHandle_t s_control;
static int64_t s_retry_at;
static esp_timer_handle_t s_timer;
static esp_err_t (*s_capture)(pogmic_stream_cb, void *);
static bool (*s_available)(void);
static esp_err_t (*s_trigger)(const char *);

/* Keep enough PSRAM to absorb short scheduling stalls while AirPlay is busy.
 * The consumer drains several microphone callbacks at once, then feeds one
 * larger block to the wake engine. */
#define WAKE_QUEUE_BYTES 65537
#define WAKE_INPUT_FRAMES 960
#define WAKE_OUTPUT_FRAMES \
  ((WAKE_INPUT_FRAMES * 16000 + CONFIG_OUTPUT_SAMPLE_RATE_HZ - 1) / \
   CONFIG_OUTPUT_SAMPLE_RATE_HZ + 2)

typedef struct {
  StreamBufferHandle_t queue;
  StaticStreamBuffer_t control;
  uint8_t *storage;
  uint32_t generation;
  atomic_bool done;
  atomic_int error;
} wake_capture_t;

static bool current(uint32_t generation) {
  portENTER_CRITICAL(&s_lock);
  bool ok = s_status.enabled && !s_suspended && generation == s_generation;
  portEXIT_CRITICAL(&s_lock);
  return ok;
}

static esp_err_t captured(const int16_t *pcm, size_t frames, uint32_t rate,
                           esp_err_t error, void *arg) {
  wake_capture_t *c = arg;
  if (!pcm) {
    atomic_store(&c->error, error);
    atomic_store(&c->done, true);
    return ESP_OK;
  }
  if (!current(c->generation)) {
    /* A user mute or a manual turn is a normal stop, not an I2S failure. */
    pogmic_stream_stop();
    return ESP_OK;
  }
  if (rate != CONFIG_OUTPUT_SAMPLE_RATE_HZ) return ESP_ERR_INVALID_ARG;
  if (xStreamBufferSend(c->queue, pcm, frames * 2, 0) != frames * 2) {
    portENTER_CRITICAL(&s_lock);
    s_status.overruns++;
    portEXIT_CRITICAL(&s_lock);
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

static void monitor(void *arg) {
  unsigned model = (unsigned)(uintptr_t)arg;
  portENTER_CRITICAL(&s_lock);
  uint32_t generation = s_worker_generation;
  portEXIT_CRITICAL(&s_lock);
  wake_capture_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  pogvoice_resampler_t *resampler = heap_caps_calloc(1, sizeof(*resampler), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  pogwake_engine_t *engine = NULL;
  bool started = false, hit = false;
  esp_err_t error = ESP_ERR_NO_MEM;
  if (!c || !resampler) goto finish;
  c->generation = generation;
  c->storage = heap_caps_malloc(WAKE_QUEUE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!c->storage) goto finish;
  c->queue = xStreamBufferCreateStatic(WAKE_QUEUE_BYTES, 2, c->storage, &c->control);
  engine = pogwake_engine_create(model);
  if (!c->queue || !engine ||
      pogvoice_resampler_init(resampler, CONFIG_OUTPUT_SAMPLE_RATE_HZ, 16000)) goto finish;
  if (!current(generation)) { error = ESP_OK; goto finish; }
  error = s_capture(captured, c);
  if (error != ESP_OK) goto finish;
  started = true;
  /* A previous queue overrun must disappear as soon as capture is healthy
   * again; otherwise the UI keeps reporting a stale detector failure. */
  portENTER_CRITICAL(&s_lock);
  if (generation == s_generation) s_status.error = ESP_OK;
  portEXIT_CRITICAL(&s_lock);
  ESP_LOGI("pogwake", "Local wake listening: %s internal free=%u largest=%u",
           pogwake_engine_phrase(model),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  while (current(generation) && !atomic_load(&c->done)) {
    int16_t pcm[WAKE_INPUT_FRAMES], converted[WAKE_OUTPUT_FRAMES];
    size_t bytes = xStreamBufferReceive(c->queue, pcm, sizeof(pcm), pdMS_TO_TICKS(50));
    if (!bytes) continue;
    size_t input_frames = bytes / sizeof(*pcm), produced = 0;
    int result = 0;
    for (size_t offset = 0; offset < input_frames; offset += 160) {
      size_t count = input_frames - offset;
      if (count > 160) count = 160;
      int n = pogvoice_resampler_process(resampler, pcm + offset, count,
                                         converted + produced,
                                         WAKE_OUTPUT_FRAMES - produced);
      if (n < 0) { result = -1; break; }
      produced += (size_t)n;
    }
    if (result == 0 && produced)
      result = pogwake_engine_feed(engine, converted, produced);
    if (result < 0) { error = ESP_FAIL; break; }
    if (result > 0) { hit = true; break; }
  }
finish:
  if (started) {
    pogmic_stream_stop();
    while (!atomic_load(&c->done)) vTaskDelay(pdMS_TO_TICKS(10));
    if (!hit && current(generation) && error == ESP_OK) error = atomic_load(&c->error);
  }
  pogwake_engine_destroy(engine);
  pogvoice_resampler_free(resampler);
  heap_caps_free(resampler);
  if (c) {
    if (c->queue) vStreamBufferDelete(c->queue);
    heap_caps_free(c->storage);
    heap_caps_free(c);
  }
  portENTER_CRITICAL(&s_lock);
  s_status.active = false;
  if (s_status.enabled && generation == s_generation) {
    s_status.error = error;
    s_pending = hit;
    if (hit) s_status.detections++;
  }
  /* Model/resampler/PCM storage are gone before the worker runs the next
   * conversation; its internal stack remains reserved and reusable. */
  s_retry_at = esp_timer_get_time() + (error == ESP_OK ? 500000 : 10000000);
  portEXIT_CRITICAL(&s_lock);
  if (hit) ESP_LOGI("pogwake", "Wake detected: %s", pogwake_engine_phrase(model));
  else if (error != ESP_OK && current(generation)) ESP_LOGW("pogwake", "Wake stopped: %s", esp_err_to_name(error));
  /* Return to the reusable audio worker. */
}

static void tick(void *arg) {
  (void)arg;
  if (xSemaphoreTake(s_control, 0) != pdTRUE) return;
  if (!s_available()) {
    portENTER_CRITICAL(&s_lock);
    if (s_status.active) s_generation++;
    s_pending = false;
    s_retry_at = esp_timer_get_time() + 1500000;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_control);
    return;
  }
  pogmic_status_t mic;
  pogmic_get_status(&mic);
  portENTER_CRITICAL(&s_lock);
  bool go = s_status.enabled && !s_suspended && !s_status.active && !mic.active && mic.clock_ready &&
             esp_timer_get_time() >= s_retry_at;
  unsigned model = s_status.model;
  bool hit = s_pending;
  if (go) {
    s_pending = false;
    if (!hit) { s_status.active = true; s_worker_generation = s_generation; }
  }
  portEXIT_CRITICAL(&s_lock);
  if (!go) { xSemaphoreGive(s_control); return; }
  esp_err_t err = ESP_OK;
  if (hit) err = s_trigger(pogwake_engine_phrase(model));
  else err = pogvoice_worker_submit(monitor, (void *)(uintptr_t)model, 4);
  if (err != ESP_OK) {
    portENTER_CRITICAL(&s_lock);
    s_status.active = false;
    s_status.error = err;
    s_retry_at = esp_timer_get_time() + 10000000;
    portEXIT_CRITICAL(&s_lock);
  }
  xSemaphoreGive(s_control);
}

void pogwake_init(esp_err_t (*capture)(pogmic_stream_cb, void *),
                  bool (*available)(void), esp_err_t (*trigger)(const char *)) {
  s_capture = capture; s_available = available; s_trigger = trigger;
  s_control = xSemaphoreCreateMutexStatic(&s_control_storage);
  s_status.supported = true;
  nvs_handle_t n;
  uint8_t enabled = 0, model = 0;
  if (nvs_open("pogwake", NVS_READONLY, &n) == ESP_OK) {
    nvs_get_u8(n, "enabled", &enabled); nvs_get_u8(n, "model", &model);
    nvs_close(n);
  }
  s_status.model = model < 3 ? model : 0;
  s_status.enabled = enabled == 1 && model < 3;
  esp_timer_create_args_t timer = {.callback = tick, .name = "pogwake"};
  if (esp_timer_create(&timer, &s_timer) != ESP_OK ||
      esp_timer_start_periodic(s_timer, 200000) != ESP_OK) {
    s_status.error = ESP_ERR_NO_MEM;
    s_status.enabled = false;
  }
}

static void pause_locked(void) {
  portENTER_CRITICAL(&s_lock);
  s_generation++;
  s_pending = false;
  s_retry_at = esp_timer_get_time() + 15000000;
  portEXIT_CRITICAL(&s_lock);
}

void pogwake_pause(void) {
  if (!s_control) return;
  xSemaphoreTake(s_control, portMAX_DELAY);
  pause_locked();
  xSemaphoreGive(s_control);
}

void pogwake_suspend(bool suspended) {
  if (!s_control) return;
  xSemaphoreTake(s_control, portMAX_DELAY);
  pause_locked();
  portENTER_CRITICAL(&s_lock);
  s_suspended = suspended;
  portEXIT_CRITICAL(&s_lock);
  xSemaphoreGive(s_control);
}

esp_err_t pogwake_configure(bool enabled, unsigned model) {
  if (!pogwake_engine_phrase(model)) return ESP_ERR_INVALID_ARG;
  if (!s_control) return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(s_control, portMAX_DELAY);
  if (enabled && (!s_timer || s_suspended || !s_available())) {
    xSemaphoreGive(s_control);
    return ESP_ERR_INVALID_STATE;
  }
  pause_locked();
  /* Disable in RAM first: an NVS error must never leave a requested mute live. */
  portENTER_CRITICAL(&s_lock);
  s_status.enabled = false;
  portEXIT_CRITICAL(&s_lock);
  nvs_handle_t n;
  esp_err_t err = nvs_open("pogwake", NVS_READWRITE, &n);
  if (err == ESP_OK) {
    err = nvs_set_u8(n, "enabled", enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(n, "model", model);
    if (err == ESP_OK) err = nvs_commit(n);
    nvs_close(n);
  }
  portENTER_CRITICAL(&s_lock);
  if (err == ESP_OK) { s_status.enabled = enabled; s_status.model = model; }
  s_status.error = err;
  s_retry_at = esp_timer_get_time() + 500000;
  portEXIT_CRITICAL(&s_lock);
  xSemaphoreGive(s_control);
  return err;
}

void pogwake_get_status(pogwake_status_t *status) {
  portENTER_CRITICAL(&s_lock);
  *status = s_status;
  portEXIT_CRITICAL(&s_lock);
  strlcpy(status->phrase, pogwake_engine_phrase(status->model), sizeof(status->phrase));
}
#endif
