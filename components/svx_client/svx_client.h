/* Inside the svxconnect client: what its web page (svx_web.c) reads and sets.
 * The reflector's name and port are the knob's radio host and port
 * (net_prov); everything else about the station lives here, in the "svx" NVS
 * namespace -- the CLI's svxconnect.conf, edited on the configuration page. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char     call[32];          /* ON6URE-KNOB: the login, and the certificate's CN */
    char     email[121];        /* goes into the certificate request */
    char     location[64];      /* shown on the reflector's map and lists */
    char     lat[16], lon[16];  /* decimal degrees; both empty = no position */
    char     sw[256];           /* switchable talkgroups: "9990, 8++, 1745+" */
    char     mon[256];          /* monitored talkgroups, same form */
    uint32_t default_tg;        /* 0 = the first switchable one */
    bool     lock_on_start;
    uint16_t linger_s;          /* hold a talkgroup this long after an over */
    uint16_t idle_s;            /* back to monitoring after this long; 0 = never */
    bool     roger;             /* a beep when an over on our talkgroup ends */
    bool     agc;               /* level the microphone before Opus */
    uint16_t tx_timeout_s;      /* 0 = none */
    bool     feed;              /* follow an enhanced reflector's feed, for where talkers are */
} svx_settings_t;

void      svx_settings_get(svx_settings_t *s);
/* Checked, stored, and applied: the client logs in again with them. Returns
 * ESP_ERR_INVALID_ARG, with a reason, for a setting that cannot work. */
esp_err_t svx_settings_set(const svx_settings_t *s, const char **why);

/* The certificate request: `start` makes the key if there is none, the request
 * from it, and keeps asking the reflector until a certificate comes back;
 * !start stops asking. */
esp_err_t svx_enroll(bool start, const char **why);
/* Delete the key, the request and the certificate. */
void      svx_forget(void);

typedef struct {
    char     phase[48];         /* "connected", "waiting for the sysop", ... */
    char     why[80];           /* why the last attempt ended, "" if it did not */
    char     server[72];        /* host:port actually used */
    int      nodes;
    uint32_t tg;
    bool     up;
} svx_state_t;

void svx_state(svx_state_t *out);
