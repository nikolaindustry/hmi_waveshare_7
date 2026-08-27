/* Desktop stand-in for the small slice of FreeRTOS the UI layer uses.
 * The simulator is single-threaded (LVGL on the main SDL loop), so the
 * critical sections and mutexes here are no-ops rather than real locks. */
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;

#define pdTRUE            1
#define pdFALSE           0
#define pdPASS            1
#define portMAX_DELAY     0xFFFFFFFFu
#define configTICK_RATE_HZ 1000

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

/* No preemption in the sim -> nothing to guard against. */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux)   do { (void)(mux); } while (0)
#define portEXIT_CRITICAL(mux)    do { (void)(mux); } while (0)
#define portENTER_CRITICAL_ISR(mux) do { (void)(mux); } while (0)
#define portEXIT_CRITICAL_ISR(mux)  do { (void)(mux); } while (0)

typedef void *SemaphoreHandle_t;
typedef void *QueueHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t t);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s);
QueueHandle_t     xQueueCreate(uint32_t len, uint32_t item_size);
BaseType_t        xQueueSend(QueueHandle_t q, const void *item, TickType_t t);
BaseType_t        xQueueReceive(QueueHandle_t q, void *item, TickType_t t);
