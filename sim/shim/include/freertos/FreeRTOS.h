#pragma once
#include <cstdint>
#include <cstddef>
typedef void* TaskHandle_t;
typedef void* SemaphoreHandle_t;
typedef void* QueueHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xFFFFFFFFu
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configASSERT(x) ((void)0)
#define portMUX_INITIALIZER_UNLOCKED {}
typedef struct { int dummy; } portMUX_TYPE;
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define taskENTER_CRITICAL(m) ((void)(m))
#define taskEXIT_CRITICAL(m) ((void)(m))
#define tskNO_AFFINITY 0x7FFFFFFF
#define configMAX_PRIORITIES 25
