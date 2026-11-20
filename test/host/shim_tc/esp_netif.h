/* Host shim for the tailchaser harness: an address, so the client takes the
 * PC's network for the knob's WiFi (the shared shim/esp_netif.h has none). */
#ifndef SHIM_TC_ESP_NETIF_H
#define SHIM_TC_ESP_NETIF_H
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key)
{
    (void)key;
    return (esp_netif_t *)1;
}
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ip)
{
    (void)n;
    ip->ip.addr = 0x0100007f;       /* 127.0.0.1 */
    return ESP_OK;
}
#endif
