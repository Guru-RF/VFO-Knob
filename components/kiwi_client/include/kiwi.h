/* The kiwi firmware's receiver, beyond what radio.h says of any radio.
 *
 * The receivers are the configuration page's list (components/sdr_rx: up to
 * four, each a KiwiSDR or a Web-888 by host:port -- or by its https:// link,
 * behind the kiwisdr.com proxy or Cloudflare -- with its passwords): the
 * same list the Icom, Xiegu, FlexRadio and UberSDR firmwares keep their web
 * SDRs in. One is in use, the dial tunes it, and another takes over at once
 * -- the swipe up, a tap on the slab, the page's In use or the API's
 * receiver= -- with no restart. Each owner's limits are kept: what refused
 * the knob is not asked again until the operator chooses it again, and a
 * receiver at its day limit at most twice more, ever (kiwi_mark.h).
 *
 * That one is the left ear. Another from the same list may play in the
 * right ear, on the same dial (sdr_rx.h, the web SDR beside a radio on the
 * other firmwares): an antenna against another. One receiver is never in
 * both ears at once. */
#ifndef KIWI_H
#define KIWI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The receiver in use: its place in sdr_get()'s list, -1 with none. */
int  kiwi_rx_active(void);

/* That one in use now, the session ended and the next one started.
 * `chosen`: the operator's act -- the dial's, the page's, the API's -- which
 * counts even for the receiver in use: one attempt through what held it
 * back, one counted try on a day-limited one. A list saved never is. False,
 * nothing done, for one not in the list, and for the right ear's. */
bool kiwi_rx_use(int i, bool chosen);

/* Its name on the dial (at most cap - 1, and 15 fit): the page's name, else
 * the antenna its /status gave this boot, else its address as sdr_rx_label
 * cuts it -- the port kept where another receiver shares its host. */
bool kiwi_rx_label(int i, char *out, size_t cap);

/* What holds it back until it is chosen again, in the page's words ("no
 * apps", "password?", "refused", ...); false when nothing does. A day-limit
 * mark is kiwi_mark's to say. */
bool kiwi_rx_hold(int i, char *word, size_t cap);

/* The receiver at this address (kiwi_hp) is the one in use, and its session
 * is on its way -- from its /status read before the login on -- or playing:
 * a Test of it answers from that, with no second login beside it (kiwi_test's
 * in_use). */
bool kiwi_rx_in_session(uint32_t hp);

/* Its audio is playing: streaming, and the squelch open -- or closed less
 * than a second, what it let through still in the ring -- or the right ear's
 * is. Flash waits for neither (at most 30 s after a change). */
bool kiwi_audible(void);

/* What the receiver in use says of itself, for the slab and the pages. */
typedef struct {
    bool    known;              /* its /status read this boot */
    char    model[12];          /* "Web-888", "KiwiSDR"; "" not known */
    char    sw[24];             /* its sw_version */
    char    antenna[48], loc[48];
    char    line2[48];          /* the slab's second line: its antenna, else its model and
                                   host:port -- the model alone when the first is host:port */
    char    line3[48];          /* ...the third: where it is, or "connecting..." */
    char    state[48];          /* "streaming", "connecting", "day limit", "time up", "moved to <host>" ... */
    int     users, users_max, ext_api;   /* -1: not said */
    int     strikes;            /* its day-limit mark's, -1 none */
    bool    held;               /* ...and its tries used up */
    bool    unsure;             /* ...a login it left unanswered, no ip_limit seen */
    double  rate, offset_khz;   /* its audio's rate, Hz; its frequency offset */
    int64_t f_max;
    bool    ovl;                /* its ADC overloaded, this last second */
} kiwi_info_t;
void kiwi_info(kiwi_info_t *out);

#endif /* KIWI_H */
