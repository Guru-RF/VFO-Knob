/* semphr.h for the PC: a mutex is a mutex. */
#pragma once
#include "freertos/FreeRTOS.h"
typedef pthread_mutex_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t m);
