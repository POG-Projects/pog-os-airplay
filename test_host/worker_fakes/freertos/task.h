#pragma once
#include "FreeRTOS.h"
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*entry)(void *), const char *name,
    uint32_t stack_bytes, void *arg, UBaseType_t priority, StackType_t *stack,
    StaticTask_t *control, BaseType_t core);
void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
