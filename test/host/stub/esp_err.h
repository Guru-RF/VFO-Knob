/* What radio.h needs of ESP-IDF's esp_err.h, for the host tests. */
#ifndef ESP_ERR_H
#define ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK                0
#define ESP_FAIL             -1
#define ESP_ERR_NOT_SUPPORTED 0x106
#endif
