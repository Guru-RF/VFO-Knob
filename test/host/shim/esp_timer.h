#ifndef SHIM_ESP_TIMER_H
#define SHIM_ESP_TIMER_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct shim_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
typedef struct {
    esp_timer_cb_t callback;
    void          *arg;
    int            dispatch_method;
    const char    *name;
    bool           skip_unhandled_events;
} esp_timer_create_args_t;
int64_t   esp_timer_get_time(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t timeout_us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);
bool      esp_timer_is_active(esp_timer_handle_t t);
#endif
