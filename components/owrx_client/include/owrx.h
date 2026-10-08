/* The openwebrx firmware's receiver, beyond what radio.h says of any radio.
 *
 * The receivers are the configuration page's list (net_prov's: up to four,
 * each an OpenWebRX or an OpenWebRX+ by its address whole -- https:// and a
 * path are fine). One is in use, the dial tunes it, and another takes over
 * at once -- the swipe up, a tap on the slab, the page -- with no restart.
 * One that cannot be reached hands over to the next in the list, as the
 * UberSDR firmware's do: the first that plays is in use from then on, and a
 * stand-in that answers only to refuse is passed by.
 *
 * A receiver is shared: its SDR is on one band for everyone listening. The
 * dial stays on that band; another is chosen from the receiver's own list
 * (BAND: its profiles), never by turning, and never two within
 * OWRX_SWITCH_GAP_MS -- OpenWebRX+ bans an address that switches quickly. A
 * band another listener chooses is followed. */
#ifndef OWRX_H
#define OWRX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The receiver in use: its place in the list, -1 with none. */
int  owrx_rx_active(void);

/* That one in use now, the session ended and the next one started. `chosen`:
 * the operator's act, which counts even for the one in use: what held it
 * back (banned, refused, a certificate) is let go of, for one attempt. False,
 * nothing done, for a place not in the list. */
bool owrx_rx_use(int i, bool chosen);

/* Its name on the dial (at most cap - 1 bytes, and 15 fit): the page's name,
 * else the one it gave this boot, else its host. */
bool owrx_rx_label(int i, char *out, size_t cap);

/* Its audio is playing: streaming, and the squelch open -- or closed less
 * than a second, what it let through still in the ring. */
bool owrx_audible(void);

/* What the receiver in use says, for the face and the pages. */
typedef struct {
    bool     known;                 /* its status.json read, or its session said */
    char     name[48];              /* its own name */
    char     version[24];           /* "v1.2.126" */
    bool     plus;                  /* OpenWebRX+ */
    int      users, users_max;      /* everyone listening, with the knob; -1 not said */
    char     band[48];              /* the band it is on, by its own name ("" not known) */
    char     line2[48];             /* the slab's second line: the band, with its SDR's name */
    char     line3[48];             /* ...the third: its software, or "connecting..." */
    char     state[48];             /* "streaming", "connecting", "banned" ... */
    float    meter_lo, meter_hi;    /* the S-meter's scale, dB (its page's) */
    int16_t  sq_db;                 /* the squelch in its dB; -150 open */
    int64_t  center;                /* where its band is: center +- rate/2; 0 not known */
    int32_t  rate;
    int      band_sel;              /* owrx_bands()'s in use, -1 not among them */
    int      band_wait_s;           /* until another band may be chosen; 0 now */
    uint32_t bands_seq;             /* moves on as the list of bands changes */
    char     url[160];              /* its address as the page has it */
} owrx_info_t;
void owrx_info(owrx_info_t *out);

/* Its bands -- the receiver's profiles -- in its own order, at most `max`:
 * how many. Each with its SDR's name and its own, and where it is (0 0 where
 * its status.json did not say). */
typedef struct {
    char    name[48];               /* its own: "40m" */
    char    sdr[40];                /* its SDR's: "RSPdx" */
    int64_t lo, hi;                 /* Hz */
} owrx_band_info_t;
int  owrx_bands(owrx_band_info_t *out, int max);

/* Band `i` of owrx_bands(), asked for: false, and nothing asked, while the
 * receiver is not playing, before owrx_info's band_wait_s is up, for the
 * band it is on already, and for one not in the list. Every listener on the
 * receiver is moved with it. */
bool owrx_band_choose(int i);

#endif /* OWRX_H */
