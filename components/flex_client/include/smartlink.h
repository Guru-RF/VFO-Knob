/* SmartLink: FlexRadio's service for reaching a radio away from home. The
 * account's radios are listed by its server, and a connection to one is
 * brokered there; after that the radio speaks its own API, over TLS.
 *
 * The account is kept as a refresh token, never the password. The radios are
 * offered on the dial after the configured ones, when enabled; the one chosen
 * is the radio in use from the next boot (radio_found_* in flex_client.c).
 */
#ifndef SMARTLINK_H
#define SMARTLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_tls.h"
#include "lwip/sockets.h"

#define SL_MAX 4

typedef struct {
    char     serial[24];
    char     name[24];          /* its nickname on the account */
    char     model[16];
    char     status[16];        /* as the server says: "Available", "In_Use" ... */
    char     ip[40];            /* its public address */
    uint16_t tls_port, udp_port;   /* 0: reachable only by hole punching */
} sl_radio_t;

/* The account, its list and the choice, from NVS. Before anything else. */
void      sl_init(void);

bool      sl_logged_in(void);
bool      sl_enabled(void);
int       sl_count(void);
bool      sl_get(int i, sl_radio_t *out);

/* The radio in use through SmartLink: its serial, or "" for a direct one. */
void      sl_active(char *serial, size_t cap);
esp_err_t sl_set_active(const char *serial);

/* The flex client's: the way to the radio in use, opened -- the server asked
 * to connect us, with `udp_port` for the radio's audio, and a TLS connection
 * to the radio with `wan validate` sent as command 1. Blocks for up to about
 * 40 s; the TLS work runs on a helper task. From a task with an internal
 * stack: a newer refresh token and a first certificate pin are saved. */
typedef struct {
    esp_tls_t         *tls;
    struct sockaddr_in udp;     /* its public UDP port */
    char               name[24];
} sl_link_t;
esp_err_t sl_open(uint16_t udp_port, sl_link_t *out, char *why, size_t cap);

/* The configuration page's: /api/smartlink, its login, logout and list. */
size_t    sl_web_endpoints(const httpd_uri_t **out);

#endif /* SMARTLINK_H */
