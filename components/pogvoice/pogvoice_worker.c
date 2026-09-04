#include "pogvoice_worker.h"
#include "sdkconfig.h"

#ifndef CONFIG_POG_VOICE
esp_err_t pogvoice_worker_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t pogvoice_worker_submit(pogvoice_audio_job_fn run, void *arg,
                                unsigned priority) {
  (void)run; (void)arg; (void)priority;
  return ESP_ERR_NOT_SUPPORTED;
}
#else
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct {
  pogvoice_audio_job_fn run;
  void *arg;
  unsigned priority;
} audio_job_t;

static StaticQueue_t s_queue_control;
static uint8_t s_queue_storage[sizeof(audio_job_t)];
static QueueHandle_t s_queue;
static StaticTask_t s_task_control;
static TaskHandle_t s_task;

static void audio_worker(void *unused) {
  (void)unused;
  for (;;) {
    audio_job_t job;
    if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
    vTaskPrioritySet(NULL, job.priority);
    job.run(job.arg);
    /* Waiting blocks the worker entirely: a reserved stack does not imply
     * microphone capture, an open connection, or continuous CPU work. */
    ESP_LOGI("pogvoice_worker", "job complete: internal free=%u largest=%u stack_min=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
  }
}

esp_err_t pogvoice_worker_init(void) {
  if (s_task) return ESP_OK;
  StackType_t *stack = heap_caps_malloc(POGVOICE_WORKER_STACK_BYTES,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!stack) return ESP_ERR_NO_MEM;
  s_queue = xQueueCreateStatic(1, sizeof(audio_job_t), s_queue_storage,
                              &s_queue_control);
  if (!s_queue) {
    heap_caps_free(stack);
    return ESP_ERR_NO_MEM;
  }
  s_task = xTaskCreateStaticPinnedToCore(audio_worker, "pogvoice_audio",
                                        POGVOICE_WORKER_STACK_BYTES, NULL, 3,
                                        stack, &s_task_control, 0);
  if (!s_task) {
    vQueueDelete(s_queue);
    s_queue = NULL;
    heap_caps_free(stack);
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI("pogvoice_worker", "Reserved reusable internal stack: %u bytes",
           (unsigned)POGVOICE_WORKER_STACK_BYTES);
  return ESP_OK;
}

esp_err_t pogvoice_worker_submit(pogvoice_audio_job_fn run, void *arg,
                                unsigned priority) {
  if (!run || priority < 3 || priority > 4) return ESP_ERR_INVALID_ARG;
  if (!s_task) return ESP_ERR_NO_MEM;
  audio_job_t job = {.run = run, .arg = arg, .priority = priority};
  return xQueueSend(s_queue, &job, 0) == pdTRUE ? ESP_OK : ESP_ERR_INVALID_STATE;
}
#endif
