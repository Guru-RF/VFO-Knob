/* queue.h for the PC: a queue of fixed-size items, under a mutex. */
#pragma once
#include "freertos/FreeRTOS.h"
typedef struct fh_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t item);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
