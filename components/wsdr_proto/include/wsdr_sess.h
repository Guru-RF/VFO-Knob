/* PA3FWM's WebSDR on the wire: one listener's session, and the files a
 * listener's page reads -- for the WebSDR firmware's receiver
 * (components/wsdr_client), on the web SDRs' link (components/websdr_link).
 *
 * A session: the WebSocket on the site's stream path (/~~stream, or
 * /~~stream?v=11 where its page opens that), with the Origin its own page
 * sends; the whole tuning at once, then mute, squelch and autonotch, as the
 * page opens; then the dial, the band, the mode and the passband followed as
 * the caller has them -- a tuning no oftener than every WSDR_TUNE_GAP_MS,
 * the last one always -- and mute and squelch as they change. Its audio
 * items decoded, a message whole at a time, brought to 24 kHz and into the
 * ring at the live point; the S-meter as it comes.
 *
 * It ends on the caller's word (WANT), on a refusal (BUSY: rate 0; an HTTP
 * status, classed), on the site's idle timeout when nothing has been touched
 * that long -- never kept up with commands of the knob's own -- or on a
 * silence. Over TLS (a front some sites put up) the same, and an http://
 * receiver's redirect to https:// on its own host is followed at once, the
 * caller told so.
 *
 * Everything runs on the caller's task, which may have its stack in PSRAM:
 * nothing here writes flash. Its buffers are one PSRAM block,
 * wsdr_sess_new()'s. */
#ifndef WSDR_SESS_H
#define WSDR_SESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "websdr_link.h"
#include "wsdr_proto.h"

/* Where a receiver is: the site's own paths go under `prefix` ("/", or a
 * front's "/websdr/"). */
typedef struct {
    const char *host;
    const char *prefix;
    uint16_t    port;
    bool        tls;
    uint32_t    key;                    /* its TLS sessions' */
} wsdr_where_t;

/* What to listen to, as the caller has it now: the tuning and the band it
 * is in (its step and limits), and who the knob is in the site's list of
 * listeners -- sent with every tuning, and a tuning sent when it changes.
 * `touch` moves on with every act of the operator's: the idle timeout's
 * clock. */
typedef struct {
    wsdr_tune_t t;
    wsdr_band_t b;
    bool        squelch, mute;
    uint32_t    touch;
    char        name[32];
} wsdr_ctl_t;

typedef enum { WSDR_ST_CONNECTING, WSDR_ST_STREAMING } wsdr_state_t;

/* For the status shade and the log. */
typedef struct {
    uint32_t sent;                      /* commands */
    uint32_t msgs, blocks, silent;      /* messages had; blocks in them; silent ones */
    uint32_t biggest;                   /* the longest message, bytes */
    uint32_t dropped;                   /* messages past WSDR_MSG_MAX, let go */
} wsdr_counts_t;

#define WSDR_MSG_MAX 16384

/* The caller's side of a session, all called on the session's task. */
typedef struct {
    void (*ctl)(void *ctx, wsdr_ctl_t *out);    /* every pass: cheap */
    void (*audio)(void *ctx, const int16_t *pcm, size_t n);     /* 24 kHz mono */
    wl_flow_cfg_t ring;
    void (*meter)(void *ctx, int dbm10);        /* as it comes, dBm x 10 */
    void (*state)(void *ctx, wsdr_state_t st, unsigned rate);
    bool (*go_on)(void *ctx);                   /* false ends it: WANT */
    void (*report)(void *ctx, const wl_report_t *r);    /* every 30 s; NULL: not told */
    void (*moved)(void *ctx, uint16_t tls_port);        /* NULL: not told */
    wsdr_counts_t *counts;              /* NULL: not counted */
    void       *ctx;
    const char *tag;
} wsdr_link_t;

typedef struct wsdr_sess wsdr_sess_t;

/* Its buffers in PSRAM, once, for every session after. NULL without memory. */
wsdr_sess_t *wsdr_sess_new(void);

/* One session, until it ends: why (never NONE). `v11` the stream path its
 * page opens; `idle_ms` its idle timeout (0 none). */
wsdr_end_t wsdr_sess_run(wsdr_sess_t *s, const wsdr_where_t *w, bool v11, uint32_t idle_ms, const wsdr_link_t *l);

/* What a page reads first, each on a connection of its own, never a session:
 * WSDR_END_NONE once it answered, else why not -- NOT_WSDR for a bandinfo.js
 * with no band in it. A redirect to https:// on its own host is followed,
 * `*tls_port` saying where (may be NULL). */
wsdr_end_t wsdr_info_read(const wsdr_where_t *w, wsdr_info_t *out, uint16_t *tls_port, bool (*go_on)(void *),
                          void *ctx, const char *tag);
/* ...its sound script: whether its page opens /~~stream?v=11. */
wsdr_end_t wsdr_path_read(const wsdr_where_t *w, bool *v11, bool (*go_on)(void *), void *ctx, const char *tag);
/* ...its page: its title, the Test's name for it. */
wsdr_end_t wsdr_title_read(const wsdr_where_t *w, char *out, size_t cap, bool (*go_on)(void *), void *ctx,
                           const char *tag);

#endif /* WSDR_SESS_H */
