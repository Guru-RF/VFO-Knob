/* esp_check.h for the PC. */
#pragma once
#include "esp_log.h"
#define ESP_RETURN_ON_FALSE(a, err, tag, fmt, ...) do {                       \
        if (!(a)) {                                                          \
            ESP_LOGE(tag, "%s(%d): " fmt, __func__, __LINE__, ##__VA_ARGS__); \
            return err;                                                      \
        }                                                                    \
    } while (0)
