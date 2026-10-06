/* task.h for the PC: a task is a thread; a tick a millisecond. */
#pragma once
#include "freertos/FreeRTOS.h"
void       vTaskDelay(TickType_t ticks);
void       vTaskDelete(TaskHandle_t t);
void       vTaskDeleteWithCaps(TaskHandle_t t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core,
                                           uint32_t caps);
