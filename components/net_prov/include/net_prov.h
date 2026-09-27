/* WiFi station bring-up and the stored AetherSDR endpoint.
 *
 * v1 seeds NVS from Kconfig on first boot so bench work can start immediately.
 * The SoftAP captive portal and the on-screen host editor come in v1.1; the
 * storage format is already what they will write to, so adding them does not
 * disturb anything above.
 */
#ifndef NET_PROV_H
#define NET_PROV_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char     ssid[33];
    char     pass[65];
    char     tci_host[64];   /* IP, or a name -- ".local" resolves via mDNS */
    uint16_t tci_port;
} vfo_cfg_t;

esp_err_t net_prov_init(void);          /* NVS + seed from Kconfig if empty */
const vfo_cfg_t *net_prov_cfg(void);

/* Start the station and keep it connected. Non-blocking. */
esp_err_t net_prov_wifi_start(void);

bool net_prov_is_connected(void);

/* Resolve the configured host. mDNS for *.local, getaddrinfo otherwise.
 * Writes a dotted-quad into `out`. */
esp_err_t net_prov_resolve(char *out, size_t out_len);

#endif /* NET_PROV_H */
