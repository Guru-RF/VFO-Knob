/* WiFi station bring-up and the stored radio endpoint.
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
    char     radio_host[64];   /* IP, or a name -- ".local" resolves via mDNS */
    uint16_t radio_port;
    /* For radios that log in (the IC-705's network user); empty otherwise. */
    char     radio_user[33];
    char     radio_pass[33];
} vfo_cfg_t;

/* Audio levels persist across reboots. Written debounced, because NVS wear is
 * real and the knob can produce a lot of intermediate values in a second. */
uint8_t net_prov_volume(void);
uint8_t net_prov_mic_gain(void);
void    net_prov_save_audio(uint8_t volume, uint8_t mic_gain);

esp_err_t net_prov_init(void);

/* Written by the HTTP configuration page. Takes effect on the next boot: the
 * transport is chosen once at startup and the TCI client has no restart path. */
esp_err_t net_prov_save_cfg(const vfo_cfg_t *cfg);

/* Credentials for the HTTP configuration page, default admin/admin. */
const char *net_prov_web_user(void);
const char *net_prov_web_pass(void);
bool        net_prov_web_is_default(void);
void        net_prov_save_web(const char *user, const char *pass);

/* How often to check for firmware updates, in hours; 0 disables. */
uint16_t net_prov_ota_hours(void);
void     net_prov_save_ota_hours(uint16_t hours);

/* Minutes of no knob, no touch and no transmit before the screen dims, and
 * before it goes dark altogether. Either 0 disables that stage. */
uint16_t net_prov_dim_min(void);
uint16_t net_prov_blank_min(void);
void     net_prov_save_dim(uint16_t dim_minutes, uint16_t blank_minutes);

/* Boot-loop guard. net_prov_boot_count() is incremented on every boot and
 * cleared once the device has been up long enough to be considered healthy;
 * call net_prov_boot_ok() from a timer for that. Three rapid boots in a row
 * put the firmware into a reduced mode so it stays reachable and flashable
 * instead of disappearing from USB in a panic loop. */
uint8_t net_prov_boot_count(void);
void    net_prov_boot_ok(void);          /* NVS + seed from Kconfig if empty */
const vfo_cfg_t *net_prov_cfg(void);

/* Start the station and keep it connected. Non-blocking. */
esp_err_t net_prov_wifi_start(void);

/* Stops and deinitialises the radio, freeing its internal RAM. Used when the
 * USB cable wins the transport choice and WiFi is dead weight. */
esp_err_t net_prov_wifi_stop(void);

bool net_prov_is_connected(void);

/* Resolve the configured host. mDNS for *.local, getaddrinfo otherwise.
 * Writes a dotted-quad into `out`. */
esp_err_t net_prov_resolve(char *out, size_t out_len);

#endif /* NET_PROV_H */
