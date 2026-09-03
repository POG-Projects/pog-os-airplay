#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint8_t StackType_t;
typedef unsigned UBaseType_t;
typedef unsigned TickType_t;
typedef int BaseType_t;
typedef struct { unsigned unused; } StaticQueue_t;
typedef struct { unsigned unused; } StaticTask_t;
typedef StaticQueue_t *QueueHandle_t;
typedef StaticTask_t *TaskHandle_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
