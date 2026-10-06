#ifndef SHIM_ESP_CHECK_H
#define SHIM_ESP_CHECK_H
#include "esp_err.h"
#include "esp_log.h"
#define ESP_RETURN_ON_FALSE(a, err, tag, ...) do { if (!(a)) { ESP_LOGE(tag, __VA_ARGS__); return err; } } while (0)
#endif
