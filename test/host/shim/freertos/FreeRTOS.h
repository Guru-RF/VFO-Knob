#ifndef SHIM_FREERTOS_H
#define SHIM_FREERTOS_H
#include <stdint.h>
/* A critical section is one process-wide recursive mutex here: the code under
 * test only needs them to exclude each other. */
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
void shim_enter(portMUX_TYPE *m);
void shim_exit(portMUX_TYPE *m);
#define taskENTER_CRITICAL(m) shim_enter(m)
#define taskEXIT_CRITICAL(m)  shim_exit(m)
#define portENTER_CRITICAL(m) shim_enter(m)
#define portEXIT_CRITICAL(m)  shim_exit(m)
typedef uint32_t TickType_t;
typedef int      BaseType_t;
#define pdPASS  1
#define pdTRUE  1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))     /* a 1 kHz tick */
#define portMAX_DELAY 0xFFFFFFFFu
#endif
