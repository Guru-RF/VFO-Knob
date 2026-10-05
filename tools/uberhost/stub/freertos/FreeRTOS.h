/* FreeRTOS.h for the PC: tasks are threads, a critical section a recursive
 * mutex (the knob's spinlocks may nest). */
#pragma once
#include <pthread.h>
#include <stdint.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define taskENTER_CRITICAL(m) pthread_mutex_lock(m)
#define taskEXIT_CRITICAL(m)  pthread_mutex_unlock(m)
typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef void    *TaskHandle_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdPASS  1
#define pdTRUE  1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFFu
