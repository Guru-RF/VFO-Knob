/* esp_system.h for the PC: a restart ends the run. */
#pragma once
void esp_restart(void);
#include "esp_err.h"
typedef void (*shutdown_handler_t)(void);
esp_err_t esp_register_shutdown_handler(shutdown_handler_t h);
