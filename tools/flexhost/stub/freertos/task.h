/* task.h for the PC: a task is a thread; a tick a millisecond -- uberhost's,
 * and the plain xTaskCreatePinnedToCore the flex task is made with. */
#pragma once
#include "freertos/FreeRTOS.h"
void       vTaskDelay(TickType_t ticks);
void       vTaskDelete(TaskHandle_t t);
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core,
                                           uint32_t caps);
BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *), const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
