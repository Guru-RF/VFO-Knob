/* A Bluetooth headset for the knob, through its second chip.
 *
 * The board carries an ESP32 beside the ESP32-S3, with classic Bluetooth --
 * which the S3 has not -- and a UART between the two. The companion firmware
 * on it (companion/) is a headset's audio gateway, as a phone is; this is the
 * knob's end of the link (bt_link_proto.h).
 *
 * While a headset is connected, the knob's audio plays in it as well as on
 * the jack, its microphone is the one keying takes, and its button -- the one
 * that answers and ends a phone call -- is a second PTT, as the glass's is:
 * a press keys, the next unkeys. */
#ifndef BT_LINK_H
#define BT_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bt_link_proto.h"
#include "esp_err.h"

esp_err_t bt_link_init(void);

typedef struct {
    bool        companion;          /* it answers */
    char        version[33];        /* its firmware's */
    uint32_t    pings, pongs, bad;  /* the link's health */
    uint32_t    rtt_us;             /* the last ping's round trip */
    btl_state_t hs;                 /* the headset, as the companion last said */
    uint32_t    presses;            /* the headset's button, all told */
    uint32_t    up_frames;          /* microphone frames in */
} bt_link_status_t;
void bt_link_status(bt_link_status_t *out);

/* The headset's audio is open: it hears the knob, and its microphone is
 * what keying sends. (While a headset is connected at all, the knob's own
 * microphone is off.) */
bool bt_link_headset_audio(void);

/* A headset is connected (its audio open or not). */
bool bt_link_headset_connected(void);

/* A connected headset has its microphone muted -- it says so as a gain of
 * 0, as the Jabras do. */
bool bt_link_headset_muted(void);

/* The boom arm as the PTT, an option on the configuration page: lowering it
 * -- the headset's microphone live -- transmits, raising it -- muted --
 * stops (app_main.c). Kept in NVS. */
bool bt_link_boom_ptt(void);
void bt_link_set_boom_ptt(bool on);

/* What the last scan found (its newest first), and whether one runs. */
int  bt_link_found(btl_found_t *out, int max);

/* The configuration page's buttons. */
void bt_link_scan(uint8_t seconds);
void bt_link_connect(const uint8_t bda[6]);
void bt_link_disconnect(void);
void bt_link_forget(const uint8_t bda[6]);

/* A press of the headset's button since the last call: the PTT's to toggle. */
bool bt_link_take_ptt(void);

#endif /* BT_LINK_H */
