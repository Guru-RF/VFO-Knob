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
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char     ssid[33];         /* the WiFi network joined last: the list's first */
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
/* A Bluetooth headset's microphone has its own gain: it comes levelled, the
 * knob's PDM element is quiet, and one setting never suited both. */
uint8_t net_prov_mic_gain_headset(void);
void    net_prov_save_audio(uint8_t volume, uint8_t mic_gain, uint8_t mic_gain_headset);
/* The same in RAM only, for net_prov_flush_audio() to write when it can: a
 * flash write holds every interrupt off for up to ~100 ms, a hole in what
 * is being heard or sent. */
void    net_prov_set_audio(uint8_t volume, uint8_t mic_gain, uint8_t mic_gain_headset);
void    net_prov_flush_audio(void);

esp_err_t net_prov_init(void);

/* The radios the knob knows -- up to NET_PROV_RADIOS, each firmware its own
 * list -- and the one in use, whose endpoint is the configuration's: the
 * client starts with it at boot. Choosing another (a swipe up, or the page)
 * takes effect on the next boot; the caller restarts the knob for that. */
#define NET_PROV_RADIOS 4
typedef struct {
    char     name[24];         /* on the dial; "" = its host */
    char     host[64];
    uint16_t port;
    char     user[33];
    char     pass[33];
} net_radio_t;
int       net_prov_radio_count(void);
int       net_prov_radio_active(void);
bool      net_prov_radio_get(int i, net_radio_t *out);
esp_err_t net_prov_radios_save(const net_radio_t *list, int n, int active);
esp_err_t net_prov_radio_activate(int i);

/* Written by the HTTP configuration page. Takes effect on the next boot: the
 * transport is chosen once at startup and the TCI client has no restart path.
 * Its ssid and pass, when they differ from the network joined last, are added
 * to the networks (below), as a configuration page from before the list
 * gives them. */
esp_err_t net_prov_save_cfg(const vfo_cfg_t *cfg);

/* The WiFi networks the knob knows -- up to NET_PROV_WIFIS: home and a
 * phone's hotspot, say -- the one joined last first. The station tries them
 * in turn until one answers, and whichever does goes to the front. The first
 * is also kept where every firmware before the list kept its one network, so
 * that one still finds it. */
#define NET_PROV_WIFIS 4
typedef struct { char ssid[33]; char pass[65]; } net_wifi_t;
int       net_prov_wifi_count(void);
/* The network the station is on, or "". */
const char *net_prov_wifi_now(void);
bool      net_prov_wifi_get(int i, net_wifi_t *out);
/* The whole list, as the configuration page gives it. */
esp_err_t net_prov_wifis_save(const net_wifi_t *list, int n);
/* One more, or a new password for one known: to the front. */
esp_err_t net_prov_wifi_add(const char *ssid, const char *pass);
/* Now and then, from a task with room on its stack: writes the list when the
 * network joined has moved to its front. */
void      net_prov_tick(void);

/* A one-time flag left in NVS ("vfo" namespace, u8 1) by something outside
 * the firmware -- tools/install-setup.sh's "sdwipe": true once, and gone. */
bool      net_prov_take_once(const char *key);

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

/* --- the knob's own hotspot, for WiFi setup (net_ap.c) ----------------------
 * A hotspot a phone can join, which sends it to the knob's page by itself (a
 * captive portal: DNS answers every name with the knob, DHCP names it as the
 * portal). The station stays up beside it. Needs net_prov_wifi_start() first. */
esp_err_t net_prov_ap_start(const char *ssid);
void      net_prov_ap_stop(void);
bool      net_prov_ap_active(void);

/* The networks in reach, strongest first, each once: at most `max`. Blocks
 * for the scan, a few seconds. */
typedef struct { char ssid[33]; int8_t rssi; bool open; } net_prov_net_t;
int net_prov_scan(net_prov_net_t *out, int max);

/* Join a network now, as given on the portal. It is tried a few times;
 * net_prov_join_state() says how it went -- and why not, when it did not --
 * and net_prov_join_keep() stores it once it worked. */
typedef enum { NET_JOIN_IDLE = 0, NET_JOIN_TRYING, NET_JOIN_OK, NET_JOIN_FAILED } net_join_t;
esp_err_t  net_prov_join(const char *ssid, const char *pass);
net_join_t net_prov_join_state(char *ssid, size_t sn, char *why, size_t wn);
esp_err_t  net_prov_join_keep(void);

/* While the hotspot is up, stop chasing the stored network: scanning the
 * channels for it takes the hotspot off the air, and the phone with it. Even
 * without this, the station stands still while a phone is on the hotspot,
 * and takes up the known networks again when the last one leaves. */
void net_prov_hold_station(bool hold);

#endif /* NET_PROV_H */
