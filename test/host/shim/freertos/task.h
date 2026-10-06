#ifndef SHIM_TASK_H
#define SHIM_TASK_H
#include <stdint.h>
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t t);
void vTaskDeleteWithCaps(TaskHandle_t t);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                           int prio, TaskHandle_t *out, int core, uint32_t caps);
#endif
