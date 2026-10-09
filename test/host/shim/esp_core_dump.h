#ifndef SHIM_ESP_CORE_DUMP_H
#define SHIM_ESP_CORE_DUMP_H
#include <stddef.h>
#include "esp_err.h"
esp_err_t esp_core_dump_image_get(size_t *addr, size_t *size);
#endif
