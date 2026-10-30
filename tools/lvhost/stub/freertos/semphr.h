/* A binary semaphore, for ui_snapshot(): one task here, so the job it
 * waits for has run by the time it looks. */
#ifndef SEMPHR_H
#define SEMPHR_H
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
typedef struct { int given; } *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return calloc(1, sizeof(int)); }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { s->given = 1; return pdTRUE; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    (void)t;
    if (!s->given) return pdFALSE;
    s->given = 0;
    return pdTRUE;
}
static inline void vSemaphoreDelete(SemaphoreHandle_t s) { free(s); }
#endif
