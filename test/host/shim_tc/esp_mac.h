/* Host shim: a MAC address of the knob's, for the name it takes on a tailnet. */
#ifndef SHIM_TC_ESP_MAC_H
#define SHIM_TC_ESP_MAC_H
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
typedef enum { ESP_MAC_WIFI_STA, ESP_MAC_WIFI_SOFTAP, ESP_MAC_BT, ESP_MAC_ETH } esp_mac_type_t;
static inline esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
    (void)type;
    static const uint8_t m[6] = { 0x24, 0x6f, 0x28, 0x12, 0xab, 0xcd };
    memcpy(mac, m, 6);
    return ESP_OK;
}
#endif
