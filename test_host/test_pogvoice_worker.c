/* Run the production dispatcher against a fragmented heap and a deterministic
 * single-slot RTOS queue. No microphone, network or firmware allocator needed.
 * cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   -I test_host/worker_fakes -I components/pogvoice/include \
 *   test_host/test_pogvoice_worker.c components/pogvoice/pogvoice_worker.c \
 *   -o /tmp/pogvoice-worker-test && /tmp/pogvoice-worker-test
 */
#include "pogvoice_worker.h"
#include "esp_heap_caps.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t largest = 38912; /* Observed failure with 77 KB total free. */
static unsigned allocations, releases, tasks, queue_deletes, job_count, depth;
static bool fail_queue, fail_task, queued;
static uint8_t *queue_storage;
static size_t queue_item_size;
static QueueHandle_t queue_handle;
static void (*worker_entry)(void *);
static void *worker_arg, *worker_stack;
static unsigned current_priority;
static jmp_buf blocked;

void *heap_caps_malloc(size_t size, unsigned caps) {
  assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  assert(size == POGVOICE_WORKER_STACK_BYTES);
  allocations++;
  return size <= largest ? malloc(size) : NULL;
}
void heap_caps_free(void *ptr) { assert(ptr); releases++; free(ptr); }
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 77843; }
size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return largest; }
void worker_test_log(const char *tag, const char *format, ...) {
  (void)tag; (void)format;
}
QueueHandle_t xQueueCreateStatic(UBaseType_t count, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *control) {
  assert(count == 1 && storage && control && !queued);
  if (fail_queue) return NULL;
  queue_storage = storage;
  queue_item_size = item_size;
  return queue_handle = control;
}
void vQueueDelete(QueueHandle_t queue) {
  assert(queue == queue_handle && !queued);
  queue_deletes++;
  queue_handle = NULL;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait) {
  assert(queue == queue_handle && wait == 0);
  if (queued) return pdFALSE;
  memcpy(queue_storage, item, queue_item_size);
  queued = true;
  return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait) {
  assert(queue == queue_handle && wait == portMAX_DELAY);
  if (!queued) longjmp(blocked, 1); /* Simulate a task blocked awaiting work. */
  memcpy(item, queue_storage, queue_item_size);
  queued = false;
  return pdTRUE;
}
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*entry)(void *), const char *name,
    uint32_t stack_bytes, void *arg, UBaseType_t priority, StackType_t *stack,
    StaticTask_t *control, BaseType_t core) {
  assert(entry && name && stack && control && core == 0 && priority == 3);
  assert(stack_bytes == POGVOICE_WORKER_STACK_BYTES);
  if (fail_task) return NULL;
  tasks++;
  worker_entry = entry; worker_arg = arg; worker_stack = stack;
  return control;
}
void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority) {
  assert(!task); current_priority = priority;
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
  assert(!task); return 17820;
}

static void unexpected(void *arg) { (void)arg; assert(!"Rejected job ran"); }
static void cycle(void *arg) {
  unsigned index = (unsigned)(uintptr_t)arg;
  assert(++depth == 1 && index == job_count++);
  assert(current_priority == 3 + index % 2);
  if (index < 999) {
    /* Enqueue a conversation before wake cleanup returns, and vice versa. */
    assert(pogvoice_worker_submit(cycle, (void *)(uintptr_t)(index + 1),
                                   3 + (index + 1) % 2) == ESP_OK);
    assert(pogvoice_worker_submit(unexpected, NULL, 3) == ESP_ERR_INVALID_STATE);
  }
  depth--;
}
static void drain(void) {
  if (setjmp(blocked) == 0) worker_entry(worker_arg);
  assert(!queued && depth == 0);
}

int main(void) {
  assert(pogvoice_worker_submit(cycle, NULL, 3) == ESP_ERR_NO_MEM);
  assert(pogvoice_worker_init() == ESP_ERR_NO_MEM);
  assert(allocations == 1 && releases == 0 && tasks == 0);
  largest = 65536;
  fail_queue = true;
  assert(pogvoice_worker_init() == ESP_ERR_NO_MEM);
  assert(releases == 1 && tasks == 0);
  fail_queue = false; fail_task = true;
  assert(pogvoice_worker_init() == ESP_ERR_NO_MEM);
  assert(releases == 2 && queue_deletes == 1 && tasks == 0);
  fail_task = false;
  assert(pogvoice_worker_init() == ESP_OK);
  assert(allocations == 4 && tasks == 1 && worker_stack);
  largest = 10240; /* Boot reservation survives even worse fragmentation. */
  assert(pogvoice_worker_init() == ESP_OK); /* Idempotent; no second stack. */
  assert(pogvoice_worker_submit(NULL, NULL, 3) == ESP_ERR_INVALID_ARG);
  assert(pogvoice_worker_submit(unexpected, NULL, 2) == ESP_ERR_INVALID_ARG);
  assert(pogvoice_worker_submit(unexpected, NULL, 5) == ESP_ERR_INVALID_ARG);
  assert(pogvoice_worker_submit(cycle, NULL, 3) == ESP_OK);
  assert(pogvoice_worker_submit(unexpected, NULL, 4) == ESP_ERR_INVALID_STATE);
  drain();
  assert(job_count == 1000 && tasks == 1 && allocations == 4 && releases == 2);
  job_count = 0;
  assert(pogvoice_worker_submit(cycle, NULL, 3) == ESP_OK);
  drain();
  assert(job_count == 1000 && allocations == 4 && tasks == 1);
  free(worker_stack); /* Harness teardown; firmware holds it until reboot. */
  puts("pogvoice worker: 2000 serialized jobs, no per-turn stack allocation");
}
