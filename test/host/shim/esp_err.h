/* Host shim: just enough of ESP-IDF for sdr_rx.c and kiwi_mark.c to run on
 * the PC against tools/mock_kiwi.py (see shim.c). */
#ifndef SHIM_ESP_ERR_H
#define SHIM_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK                 0
#define ESP_FAIL               -1
#define ESP_ERR_NO_MEM         0x101
#define ESP_ERR_INVALID_ARG    0x102
#define ESP_ERR_INVALID_STATE  0x103
#define ESP_ERR_INVALID_SIZE   0x104
#define ESP_ERR_NOT_FOUND      0x105
#define ESP_ERR_NOT_SUPPORTED  0x106
#define ESP_ERR_NVS_NOT_FOUND  0x1102
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
const char *esp_err_to_name(esp_err_t e);
#endif
