/* Host shim: no network interfaces of the knob's own on the PC. */
#ifndef SHIM_ESP_NETIF_H
#define SHIM_ESP_NETIF_H
#include <stdint.h>
#include "esp_err.h"
typedef struct esp_netif_obj esp_netif_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
#define IPSTR "%d.%d.%d.%d"
#define esp_ip4_addr1(a) (((const uint8_t *)(&(a)->addr))[0])
#define esp_ip4_addr2(a) (((const uint8_t *)(&(a)->addr))[1])
#define esp_ip4_addr3(a) (((const uint8_t *)(&(a)->addr))[2])
#define esp_ip4_addr4(a) (((const uint8_t *)(&(a)->addr))[3])
#define IP2STR(a) esp_ip4_addr1(a), esp_ip4_addr2(a), esp_ip4_addr3(a), esp_ip4_addr4(a)
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { (void)key; return 0; }
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *ip)
{
    (void)n; (void)ip;
    return ESP_FAIL;
}
#endif
