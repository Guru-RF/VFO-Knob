/* A web SDR as a second receiver beside the radio's own: a KiwiSDR, a
 * Web-888, or an UberSDR's Kiwi input -- all three speak KiwiSDR's protocol,
 * a WebSocket carrying text commands up and IMA-ADPCM audio down.
 *
 * The receiver follows the radio: its frequency, mode and passband, retuned
 * as the dial turns. Its audio goes to audio_out's second source -- the right
 * ear, the radio in the left -- and its S-meter reading comes with it.
 *
 * The list of receivers is kept in NVS, up to SDR_MAX of them, and chosen on
 * the dial (a swipe down) or on the configuration page, which can test each.
 */
#ifndef SDR_RX_H
#define SDR_RX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SDR_MAX 4

typedef struct {
    char     name[24];      /* on the dial; "" = the receiver's own name */
    char     host[64];
    uint16_t port;
    char     pass[32];      /* the receiver's user password; "" = none */
    char     ipl[32];       /* its time-limit exemption password; "" = none */
} sdr_cfg_t;

/* The list, from NVS at boot (sdr_rx_init) and whenever it is saved. */
esp_err_t sdr_rx_init(void);
int       sdr_count(void);
bool      sdr_get(int i, sdr_cfg_t *out);
esp_err_t sdr_save(const sdr_cfg_t *list, int n);

/* Listen to receiver `i`, or to none (-1). Kept across a restart. */
void sdr_rx_select(int i);
int  sdr_rx_selected(void);

/* The mix, as audio_out takes it: -100 the radio alone, 0 the radio left and
 * the SDR right, +100 the SDR alone. Kept across a restart too. */
void   sdr_rx_set_balance(int8_t balance);
int8_t sdr_rx_balance(void);

/* The radio, as the receiver should follow it: Hz, the radio's mode name
 * ("usb", "cwr", "digu" ...) and its passband relative to the carrier. Cheap:
 * the receiver is retuned only when something changed. */
void sdr_rx_tune(int64_t hz, const char *mode, int32_t lo, int32_t hi);

typedef struct {
    int   sel;              /* -1 none */
    bool  streaming;        /* logged in, audio arriving */
    bool  trouble;          /* not reached, refused, busy ... */
    char  note[12];         /* the trouble in a word, for the dial */
    char  state[24];        /* "connecting", "streaming", "busy", "wrong password" ... */
    char  name[24];         /* what the dial calls it */
    float smeter_dbm;
} sdr_status_t;
void sdr_rx_status(sdr_status_t *out);

/* The configuration page's test of one receiver: its /status, then a login,
 * and whether its owner lets apps listen (a KiwiSDR's ext_api_nchans; the
 * knob, without a waterfall, is one). Blocks for up to about 15 s; the result
 * as JSON into `json`. */
esp_err_t sdr_test(const sdr_cfg_t *c, char *json, size_t cap);

#endif /* SDR_RX_H */
