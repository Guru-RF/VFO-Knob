#ifndef SHIM_ESP_FLASH_H
#define SHIM_ESP_FLASH_H
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_flash_t esp_flash_t;
esp_err_t esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length);
#endif
