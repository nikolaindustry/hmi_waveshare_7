/* Desktop stand-in for freertos/task.h.
 *
 * vTaskDelay() genuinely sleeps AND advances the LVGL tick, so UI code
 * that delays before esp_restart() (the role/baud screens do) behaves
 * sanely instead of freezing the render loop. */
#pragma once
#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

void       vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
void       vTaskDelete(TaskHandle_t t);

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name,
                                   uint32_t stack, void *arg, uint32_t prio,
                                   TaskHandle_t *handle, int core);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, uint32_t prio, TaskHandle_t *handle);
