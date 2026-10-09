/* The WebSDR firmware's receiver, beyond radio.h: its list, its bands, and
 * what the face and the pages show of it. components/wsdr_client implements
 * radio.h over PA3FWM's WebSDR protocol (components/wsdr_proto).
 *
 * Its receivers are the configuration page's list (net_prov, up to four,
 * each its whole address: "http://websdr.ewi.utwente.nl:8901/"); one is in
 * use, another taken over at once, as on the OpenWebRX firmware -- one not
 * reached handing over to the next, the first that plays in use from then
 * on. A site too busy is tried again by itself after five minutes, as its
 * own page bids a listener; one that is no WebSDR, refuses the knob or sent
 * it elsewhere waits until chosen again.
 *
 * A WebSDR tunes each listener independently: a band chosen moves nobody else.
 * So the dial runs on from one of a site's bands into the next by itself,
 * and BAND lists its bands -- or, on a site of one wide band (Twente), the
 * band plan its page carries, as jumps. Its idle timeout, where it has one,
 * is kept as its page keeps it: nothing touched that long, the knob lets go
 * of it (IDLE), and the next touch takes it up again. */
#ifndef WSDR_H
#define WSDR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* The receiver in use, as the session has it -- a stand-in that plays
 * already, before the list is saved: its place in the list, -1 none. */
int  wsdr_rx_active(void);
/* Receiver `i` in use, now. `chosen`: the operator's act, which also lets
 * go of whatever held it. False: no such receiver. */
bool wsdr_rx_use(int i, bool chosen);
/* Its name on the dial: the page's, else its own (its page's title), else
 * its host. */
bool wsdr_rx_label(int i, char *out, size_t cap);
/* Its audio heard now: playing, not muted, its squelch open. */
bool wsdr_audible(void);

typedef struct {
    bool     in_use, playing, held;
    int      wait_s;                /* not again by itself for this long; 0 none */
    char     why[16];               /* ...for this ("BUSY"); "" with neither */
    char     name[64];              /* its page's title, "" not read yet */
    uint16_t tls_port;              /* its redirect to https:// followed: there; 0 none */
} wsdr_rx_state_t;
bool wsdr_rx_state(int i, wsdr_rx_state_t *out);

/* The page's Test: its bandinfo.js, its page's title and its sound script,
 * three small GETs -- never a session, so never a listener's place taken. */
typedef struct {
    bool ok;                        /* a WebSDR answered */
    char error[16];                 /* why not: the face's word */
    char name[64];                  /* its page's title */
    int  bands;                     /* its bands */
    int  plan;                      /* ...and, on one wide band, its band plan's */
    int  idle_min;                  /* its idle timeout, minutes; 0 none */
    bool v11;                       /* its page opens /~~stream?v=11: a distributed server */
    char url[80];                   /* where it answered, as the list keeps it: "" where asked */
} wsdr_test_t;
bool wsdr_test(const char *addr, wsdr_test_t *out);

/* What the face's slab and the radio page show. */
typedef struct {
    bool     known;                 /* its bandinfo.js read */
    char     name[64];
    char     band[24];              /* the band the dial is in, by the site's name ("80m", "hf") */
    char     line2[48];             /* the slab's second line */
    char     line3[48];             /* ...the third: "WebSDR", or "connecting..." */
    char     state[48];             /* "streaming", "busy, again in 300 s" ... */
    unsigned rate;                  /* its audio's rate now, Hz; 0 not playing */
    bool     sam;                   /* it plays AM sync: Twente's server, not a distributed one */
    int      idle_min;              /* its idle timeout; 0 none */
    int      band_sel;              /* wsdr_bands()'s the dial is in; -1 none */
    uint32_t bands_seq;             /* moves on as the list changes */
    int64_t  lo, hi;                /* the band the dial is in, Hz */
    char     url[160];
} wsdr_now_t;
void wsdr_now(wsdr_now_t *out);

/* BAND's list: the site's bands -- or its band plan's ranges, where it has
 * one band only. */
typedef struct {
    char    name[24];
    int64_t lo, hi;                 /* Hz */
    bool    plan;                   /* a range of its band plan, not a band of its own */
} wsdr_choice_t;
int  wsdr_bands(wsdr_choice_t *out, int max);
/* The dial onto choice `i`: where it was last in that band, else where the
 * site starts one -- a plan's range at its middle, in AM on a broadcast band. */
bool wsdr_band_choose(int i);

/* Who the knob is in a site's list of listeners -- the call sign, say; ""
 * for no name, as a page with none sends. One name for every web receiver:
 * the Kiwi firmwares' too (NVS "sdrid"). Control characters left out, and
 * spaces at either end; 31 bytes at most, a character cut at the end left
 * out whole. A site is told with the next tuning. Saving writes flash: the
 * web server's task. */
void      wsdr_ident(char *out, size_t cap);
esp_err_t wsdr_ident_save(const char *who);

#endif /* WSDR_H */
