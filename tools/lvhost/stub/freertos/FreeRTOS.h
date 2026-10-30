/* FreeRTOS's names ui.c meets: on the host there is one task, the
 * harness's -- the critical sections are esp_lvgl_port.h's. */
#ifndef FREERTOS_H
#define FREERTOS_H
#include <stdint.h>
typedef int      BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#endif
