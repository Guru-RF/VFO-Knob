/* OpenWebRX's protocol on the wire: one session with a receiver, and its
 * status.json -- for the OpenWebRX firmware's receiver (components/
 * owrx_client), on the web SDRs' link (components/websdr_link).
 *
 * A session: the WebSocket on <path>ws/, the hello and the connection's
 * properties; the receiver's band (its config) read, the dial kept on it or
 * moved to the band's own start, the DSP's params and start; then the dial,
 * the mode, the passband and the squelch followed as the caller has them,
 * the audio decoded, brought to 24 kHz and into the ring at the live point,
 * the S-meter, the waterfall read and let go. A band the caller chooses is
 * asked for (selectprofile) never within OWRX_SWITCH_GAP_MS of the last, or
 * of the session's start; a band another listener chose is followed.
 *
 * Every refusal is classed (owrx_end_t): the HTTP status of the upgrade, a
 * backoff and its reason, an sdr_error, no hello, a silence. Over TLS
 * (https://, wss://) the same, and an http:// receiver's redirect to
 * https:// on its own host is followed at once, the caller told so.
 *
 * Everything runs on the caller's task, which may have its stack in PSRAM:
 * nothing here writes flash. Its buffers are one PSRAM block,
 * owrx_sess_new()'s. */
#ifndef OWRX_SESS_H
#define OWRX_SESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "owrx_proto.h"
#include "websdr_link.h"

/* What to listen to, as the caller has it now. A band is asked for when
 * `band_seq` moves on: `band` its id. */
typedef struct {
    owrx_tune_t t;
    uint8_t     sq_pct;                 /* the squelch, 0-100 %: 0 open */
    uint32_t    band_seq;
    char        band[OWRX_ID_MAX];
} owrx_ctl_t;

typedef enum { OWRX_ST_CONNECTING, OWRX_ST_CONNECTED, OWRX_ST_STREAMING } owrx_state_t;

/* For the status shade and the log: text frames sent; frames had, by kind;
 * the biggest; audio lost (bytes the decoder passed over). */
typedef struct {
    uint32_t sent, texts, audio, fft, other;
    uint32_t biggest;                   /* the longest text frame, bytes */
    uint32_t lost;
} owrx_counts_t;

/* The caller's side of a session, all called on the session's task. */
typedef struct {
    /* The newest settings: called every pass, so cheap. */
    void (*ctl)(void *ctx, owrx_ctl_t *out);
    /* Where it listens has changed -- the first band, the caller's, another
     * listener's: the caller moves its dial onto the span as it will
     * (owrx_retune), before the session tunes again from ctl(). */
    void (*span)(void *ctx, const owrx_said_t *said);
    /* What else it said (OWRX_EV_*): its name, its bands, its listeners, a
     * log message. NULL: not told. */
    void (*told)(void *ctx, uint32_t ev, const owrx_said_t *said);
    /* Its audio, 24 kHz mono. */
    void (*audio)(void *ctx, const int16_t *pcm, size_t n);
    /* The ring that audio goes into (websdr_link.h's flow). */
    wl_flow_cfg_t ring;
    /* Its S-meter, dB as its page shows it, about four times a second. */
    void (*meter)(void *ctx, float db);
    void (*state)(void *ctx, owrx_state_t st, const owrx_said_t *said);
    /* False ends it: WANT. Called every pass (50 ms). */
    bool (*go_on)(void *ctx);
    /* Every 30 s while it plays: how the audio came. NULL: not told. */
    void (*report)(void *ctx, const wl_report_t *r);
    /* An http:// receiver's redirect to https:// on its own host, followed:
     * the caller keeps https for it, on `tls_port`. NULL: not told. */
    void (*moved)(void *ctx, uint16_t tls_port);
    owrx_counts_t *counts;              /* NULL: not counted */
    void       *ctx;
    const char *tag;                    /* for the log */
} owrx_link_t;

typedef struct owrx_sess owrx_sess_t;

/* Its buffers in PSRAM, once, for every session after. NULL without memory. */
owrx_sess_t *owrx_sess_new(void);

/* One session, until it ends: why (never NONE). `u` is where it is -- its
 * TLS and port as the caller keeps them -- and `key` its TLS sessions' key
 * (owrx_key). `said` is what it was told, filled in as it goes. */
owrx_end_t owrx_sess_run(owrx_sess_t *s, const owrx_url_t *u, uint32_t key, const owrx_link_t *l,
                         owrx_said_t *said);

/* Its status.json -- one small GET on a connection of its own, never a
 * session: OWRX_END_NONE and `out` once it answered (out->ok says whether as
 * OpenWebRX's), else why not. A redirect to https:// on its own host is
 * followed, `*tls_port` saying where (may be NULL). */
owrx_end_t owrx_status_read(const owrx_url_t *u, uint32_t key, owrx_status_t *out, uint16_t *tls_port,
                            bool (*go_on)(void *), void *ctx, const char *tag);

#endif /* OWRX_SESS_H */
