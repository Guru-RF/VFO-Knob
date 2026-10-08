/* A web SDR's link: one WebSocket to a receiver on the internet, in the clear
 * or over TLS, its frames read as they come -- of any length, a piece at a
 * time -- and an HTTP GET beside it; and the audio's way into the knob's
 * ring, kept at the live point.
 *
 * Lifted from KiwiSDR's session (components/kiwi_proto/kiwi_sess.c), what of
 * it is no Kiwi's: every address a name has, tried in turn; a connection,
 * its TLS a step at a time (kiwi_tls.h) and the caller asked between; the
 * upgrade on any path, with the Host a front routes on; an http:// receiver's
 * redirect to https:// on its own host followed at once, once, the caller
 * told so to keep https for it -- any other redirect is not. OpenWebRX's
 * session runs on it (components/owrx_client); the Kiwi's will, once moved
 * here behind its own mock's tests.
 *
 * Its reader passes every frame on in pieces, as much as has come: a frame
 * of hundreds of kB -- OpenWebRX+'s bookmarks -- costs no more memory than
 * one of ten bytes, and the caller, which knows a frame by its first bytes,
 * reads on or lets the rest go by. A ping is answered by itself.
 *
 * Everything runs on the caller's task, which may have its stack in PSRAM:
 * nothing here writes flash. Its buffers are one PSRAM block, wl_new()'s. */
#ifndef WEBSDR_LINK_H
#define WEBSDR_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kiwi_proto.h"

/* How an attempt ended, or why none could start. */
typedef enum {
    WL_OK,              /* connected, upgraded (101) -- or the GET's 200 */
    WL_WANT,            /* the caller let go of it meanwhile */
    WL_NOT_FOUND,       /* the name looked up to nothing */
    WL_NO_ROUTE,
    WL_NO_SOCKET,       /* none free on the knob, or no memory for TLS */
    WL_NO_ANSWER,       /* no connection, no TLS, or no HTTP answer in 5 s */
    WL_CERT,            /* its certificate does not verify: not spoken to */
    WL_HTTP,            /* answered with another status: wl_said_t's */
    WL_MOVED,           /* a redirect not followed: wl_said_t's `to` */
} wl_end_t;

/* A receiver: its address, over TLS or not, and the key its TLS sessions are
 * kept under for the next connection (owrx_key(), kiwi_hp()). */
typedef struct {
    const char *host;
    uint16_t    port;
    bool        tls;
    uint32_t    key;
} wl_addr_t;

/* The caller, asked between every slice of a wait (100 ms) and every step
 * of a TLS handshake: false lets go (WL_WANT). NULL: never asked. */
typedef struct {
    bool      (*go_on)(void *ctx);
    void       *ctx;
    const char *tag;                    /* for the log */
} wl_ask_t;

/* What an attempt came to, whatever it ended in. */
typedef struct {
    int      status;                    /* the HTTP status; 0 none */
    bool     reached;                   /* a connection was made at all */
    uint16_t tls_port;                  /* a redirect to https:// followed: TLS spoken here */
    char     to[64];                    /* ...one not followed: where it pointed */
} wl_said_t;

typedef struct wl wl_t;

/* Its buffers, in PSRAM, for every connection after. NULL without memory. */
wl_t *wl_new(void);

/* Connected to the receiver and upgraded on `path` ("/OWRX/ws/"): WL_OK; or
 * why not, the first address that answered HTTP counting over those that
 * did not answer at all. A redirect to https:// on its own host is followed
 * (`said->tls_port`). */
wl_end_t wl_open(wl_t *w, const wl_addr_t *a, const char *path, const wl_ask_t *k, wl_said_t *said);

/* A frame to the receiver, masked as a client's are: text (0x1), a close
 * (0x8), a pong. 1 KB at most. False: the connection failed. */
bool wl_send(wl_t *w, uint8_t op, const void *p, size_t n);
bool wl_text(wl_t *w, const char *t);

/* A piece of a frame, as much of it as has come. A message the receiver
 * sent in fragments comes as one: its continuations carry its first's op. */
#define WL_TEXT 0x1
#define WL_BIN  0x2
typedef struct {
    uint8_t        op;                  /* WL_TEXT, WL_BIN */
    bool           first, last;         /* the frame's first piece; its last */
    uint64_t       len;                 /* the frame's length */
    const uint8_t *p;                   /* valid until the next wl_read() */
    size_t         n;
} wl_piece_t;

/* The next piece, waiting up to `ms` for one: 1 a piece; 0 none yet; -2 the
 * receiver closed (a close frame, or its end of the socket -- after
 * everything it sent before); -1 failed. A ping is answered and passed by. */
int wl_read(wl_t *w, int ms, wl_piece_t *pc);

/* Whether more is here already, or comes within `ms`. */
bool wl_more(wl_t *w, int ms);

/* The connection closed -- a close frame first, where it still goes, when
 * `bye`. The buffers are kept for the next. */
void wl_close(wl_t *w, bool bye);

/* Bytes in and out over the connection's life, for the log. */
void wl_counts(const wl_t *w, uint64_t *in, uint64_t *out);

/* An HTTP GET of `path` on a connection of its own -- a redirect to https://
 * on its own host followed as for wl_open -- its body to `body` a piece at a
 * time, `max` bytes at most: WL_OK on a 200, WL_HTTP on any other status,
 * or why there was none. Read to the receiver's close or its Content-Length;
 * 5 s of silence ends it. */
wl_end_t wl_get(const wl_addr_t *a, const char *path, void (*body)(void *ctx, const uint8_t *p, size_t n),
                void *bctx, size_t max, const wl_ask_t *k, wl_said_t *said);

/* ------------------------------------------------------------- the audio */

/* The audio's way into the ring, block by block -- KiwiSDR's session's ring
 * discipline (kiwi_sess.c's flow()), for any receiver: a block's lateness
 * against where it stands in the stream; the ring kept near its target, the
 * receiver's clock followed with the drift trim; after a stall the ring
 * could not ride out, the blocks held up meanwhile -- handed over at once, or
 * at least twice as fast as they play -- left out in one jump, whole ones,
 * oldest first, where the stall's silence already is; a backlog handed over
 * more slowly cut back to the target each time it would fill the ring past
 * 80 %. With `target_max` over `target`, a stream that keeps breaking up
 * grows the target to ride its stalls out, and calm a while it eases back.
 *
 * A block is the caller's: a SND frame of a Kiwi's; for a receiver whose
 * frames come in all sizes -- OpenWebRX's -- its audio gathered into blocks
 * of WL_BLOCK samples at 24 kHz, so a stall is told from the stream's own
 * unevenness as it is for a Kiwi's 171 ms. */
#define WL_OUT_HZ 24000
#define WL_BLOCK  2048                  /* 85 ms at 24 kHz */

typedef struct {
    /* The ring the audio goes into: the samples it holds now. */
    size_t   (*queued)(void *ctx);
    size_t     target, room;
    bool       trim;                    /* follow the receiver's clock (kiwi_dsp_trim) */
    size_t     target_max;              /* over `target`: the network's to grow it */
    void     (*preroll)(void *ctx, size_t n);
    uint32_t (*underruns)(void *ctx);
    void      *ctx;
    const char *tag;
} wl_flow_cfg_t;

/* How the audio came, these last 30 s: for the log. */
typedef struct {
    uint32_t blocks;
    uint32_t gap_ms;                    /* the longest between two arriving */
    uint32_t lost;                      /* blocks that never came (a Kiwi's numbers missing) */
    uint32_t held;                      /* blocks late with none missing: the network held them */
    uint32_t jumps, left_ms;            /* to the live point; what they left out */
    uint32_t breaks;                    /* stalls the ring could not ride out */
    uint32_t level_ms, target_ms;       /* the ring's average level; its target now */
    float    trim;
} wl_report_t;

enum { WL_JUMP_NONE, WL_JUMP_STALE, WL_JUMP_FULL };
typedef struct {
    float    avg;
    int      fed;
    uint8_t  jump;
    uint32_t left, jump_n;
    int64_t  t_jump;
    bool     silent, refill, any;
    uint32_t seq;
    int64_t  t_last, t_rep;
    double   pos_us, period_us, ref_us;
    int64_t  t_ref;
    double   usual_us, first_us;
    size_t   tgt, roof;
    int64_t  t_brk, t_ease;
    uint32_t ur;
    wl_report_t rep;
} wl_flow_t;

/* A session's flow from nothing: at the link's own target (a grown one was
 * the last receiver's network's), the ring's pre-roll set to it. */
void wl_flow_init(wl_flow_t *w, const wl_flow_cfg_t *l);

/* A block in: `seq` its number (one more than the last, unless some never
 * came), `len` its samples at 24 kHz, `period_us` how long they last; `now`
 * when it came. True: play it; false: leave it out -- decoded all the same.
 * `more` says whether more of the stream is here already (NULL: never).
 * `trim`: the resampler whose drift trim follows the ring (NULL: none). */
bool wl_flow(wl_flow_t *w, const wl_flow_cfg_t *l, kiwi_dsp_t *trim, uint32_t seq, size_t len, double period_us,
             int64_t now, bool play, bool (*more)(void *mctx), void *mctx);

/* These 30 s, said and started afresh; `trim` the resampler's (0: none). */
void wl_flow_report(wl_flow_t *w, wl_report_t *out, float trim, int64_t now);

#endif /* WEBSDR_LINK_H */
