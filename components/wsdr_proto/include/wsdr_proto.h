/* PA3FWM's WebSDR -- the receivers listed on websdr.org -- the parts of its
 * protocol that are plain C: its audio items decoded, its bandinfo.js read as
 * it streams, the tuning the knob sends, and what its page makes of the
 * S-meter. Nothing in here touches ESP-IDF, so test/host builds and checks it
 * on the PC.
 *
 * The server software is closed; the protocol is what its pages' JavaScript
 * does, written up in WEBSDR-PROTOCOL.md (§n below) from archived copies and
 * from open clients and servers. This is an independent implementation of
 * that behaviour, for interoperability: none of the pages' code is in here.
 *
 * One WebSocket per listener, /~~stream (/~~stream?v=11 on the distributed
 * servers, dist11, which every site but Twente runs). The knob's tuning goes
 * up it as text, "GET /~~param?f=...&band=...", the audio comes down it as
 * binary items. Each listener tunes independently: a band chosen moves
 * nobody else, and the waterfall is a socket of its own the knob never
 * opens. */
#ifndef WSDR_PROTO_H
#define WSDR_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------ the audio */

/* A message is a run of items, each from a byte boundary, none split across
 * two messages (§3.1):
 *   0xF0-0xFF  2 bytes  the S-meter, 12 bits
 *   0x80       1+128    128 samples of G.711 A-law
 *   0x90-0xDF  a block  128 coded samples, the coder's width I = 14 - (b >> 4)
 *   0x00-0x7F  a block  128 coded samples, I as before
 *   0x81       3        the sample rate, Hz, big-endian; 0 = refused, busy
 *   0x82       3        the quantiser's step, big-endian
 *   0x83       2        the conversion byte: 0x10 no integrator (SSB, CW);
 *                       its low nibble the page's output low-pass
 *   0x84       1        128 samples of silence (muted, or its squelch shut)
 *   0x85       7        AM sync: the carrier's lock and true frequency, mHz
 *   0x86       1        resync: the page re-weighs its playout delay
 *   0x87       7        the server's clock
 * and anything else one byte, passed by. A coded block has no length: it
 * ends where its 128th sample does, the rest of that byte let go (§3.2). */
#define WSDR_BLOCK 128

/* The decoder: a 20-tap adaptive predictor, its history kept doubled -- the
 * step's half is a half where the step is odd -- and the integrator AM and
 * FM run through (§3.3). Rate, step and conversion hold until changed. */
typedef struct {
    int32_t  tap[20];
    int64_t  hist2[20];             /* 2x: a step's half kept whole */
    int64_t  integ;
    uint32_t step;
    uint8_t  conv;
    uint8_t  width;                 /* the coder's I, 1..5 */
} wsdr_dec_t;

/* What the items say, as they come, in their order in the message. */
typedef struct {
    void (*audio)(void *ctx, const int16_t *pcm, bool silent);   /* WSDR_BLOCK samples */
    void (*rate)(void *ctx, unsigned hz);                       /* 0: refused */
    void (*meter)(void *ctx, int v);                            /* wsdr_dbm() */
    void (*carrier)(void *ctx, uint64_t mhz, int lock);         /* 0 locked, 1 locking, 2 not */
    void (*resync)(void *ctx);
    void *ctx;
} wsdr_cb_t;

void wsdr_dec_reset(wsdr_dec_t *d);
/* One binary message, whole: its items to `cb`. Bytes past its end read as
 * zeros, as the page's own reading does. The number of blocks it held. */
size_t wsdr_items(wsdr_dec_t *d, const uint8_t *m, size_t n, const wsdr_cb_t *cb);

/* The S-meter's 12 bits as the page shows them: dBm x 10, v - 1270 (§6),
 * calibrated as far as the site's owner calibrated it. */
static inline int wsdr_dbm10(int v) { return v - 1270; }
/* ...on the page's own scale, S9 = -73 dBm and 6 dB an S-unit (S1 at -121):
 * S-units x 10 up to S9 (90), and above it 90 + the dB over S9. */
#define WSDR_S9_DBM10 (-730)
int wsdr_s10(int dbm10);

/* ------------------------------------------------------------ the bands */

/* /tmp/bandinfo.js, written by the server: JavaScript, not JSON, and 64 kB on
 * Twente, nearly all of it the waterfall's scale image names. Read as it
 * streams, a piece at a time wherever the reads break it; only the numbers
 * below kept (§4.1). Frequencies are the file's kHz, in Hz here. */
#define WSDR_BANDS   16
#define WSDR_PLAN    40

typedef struct {
    int64_t center_hz;
    int64_t span_hz;                /* its samplerate: it covers center +- span/2 */
    int64_t vfo_hz;                 /* where the page starts on it */
    double  step_hz;                /* its tuningstep: the server's resolution */
    int32_t maxbw_hz;               /* maxlinbw: no passband edge past 0.95 x this */
    char    name[16];
} wsdr_band_t;

typedef struct {
    int64_t lo_hz, hi_hz;
} wsdr_range_t;

typedef struct {
    int          n_bands;           /* those read, WSDR_BANDS at most */
    int          nbands;            /* what it says it has */
    wsdr_band_t  band[WSDR_BANDS];
    int          n_plan;            /* freqbands[]: Twente's band plan, for BAND */
    wsdr_range_t plan[WSDR_PLAN];
    int64_t      ini_hz;            /* -1: none */
    char         ini_mode[8];
    uint32_t     idle_ms;           /* 0: no idle timeout */
} wsdr_info_t;

/* The reader's own state, between pieces. */
typedef struct {
    wsdr_info_t *out;
    uint8_t      st, depth, ctx, quote;
    uint8_t      in_rec;
    char         tok[48];
    uint8_t      tn;
    char         key[16];
    bool         want_val;
    wsdr_band_t  cur;
    wsdr_range_t rng;
    uint8_t      have;              /* the record's fields seen */
} wsdr_info_rd_t;

void wsdr_info_begin(wsdr_info_rd_t *r, wsdr_info_t *out);
void wsdr_info_feed(wsdr_info_rd_t *r, const uint8_t *p, size_t n);
/* The file read to its end: false if it held no band at all. */
bool wsdr_info_end(wsdr_info_rd_t *r);

/* The band covering `hz`: the one it is in, else the first within 4 kHz of
 * its edge, as the page chooses (§4.2); -1 none. `prefer` first, where it
 * covers it. */
int wsdr_band_of(const wsdr_info_t *in, int64_t hz, int prefer);

/* ------------------------------------------------------- how it ended */

typedef enum {
    WSDR_END_NONE,                  /* still on */
    WSDR_END_WANT,                  /* the knob let go: another chosen, the list saved */
    WSDR_END_NOT_FOUND,             /* the name looked up to nothing */
    WSDR_END_NO_ROUTE,
    WSDR_END_NO_SOCKET,             /* none free on the knob */
    WSDR_END_NO_ANSWER,             /* no connection, no TLS, no HTTP answer */
    WSDR_END_CERT,                  /* its certificate does not verify */
    WSDR_END_MOVED,                 /* a redirect not followed */
    WSDR_END_NOT_WSDR,              /* no bandinfo.js, a 404 on the stream: no WebSDR there */
    WSDR_END_REFUSED,               /* 401, 403: its Origin, its address */
    WSDR_END_DOWN,                  /* 5xx */
    WSDR_END_BUSY,                  /* "too busy right now": rate 0 */
    WSDR_END_IDLE,                  /* its idle timeout: nothing touched that long */
    WSDR_END_QUIET,                 /* nothing at all for a while */
    WSDR_END_CLOSED,                /* it closed the stream */
    WSDR_END_PROTOCOL,              /* a rate the knob cannot play, an item it cannot read */
} wsdr_end_t;
/* The face's word for it, 28 pt within 264 px: "BUSY", "NOT A WEBSDR". */
const char *wsdr_end_word(wsdr_end_t e);
/* The upgrade's answer by its HTTP status: 101 in (NONE); 401, 403 REFUSED;
 * 404 and the rest NOT_WSDR; 5xx DOWN; a redirect MOVED; 0 NO_ANSWER. */
wsdr_end_t wsdr_http(int status);
/* How long before trying a receiver again after `e`, the `tries`-th time in a
 * row (from 1), ms before jitter: BUSY 5 min, as its page bids a listener
 * try later; 2, 4 ... 512 s after the rest. 0: not by itself -- held until
 * chosen again (CERT, MOVED, NOT_WSDR, REFUSED), or, IDLE, until the dial is
 * touched. */
uint32_t wsdr_retry_ms(wsdr_end_t e, unsigned tries);

/* ------------------------------------------------------ the site's name */

/* Its page's <title>, read as it streams: the Test's name for a receiver
 * ("Wide-band WebSDR in Enschede, the Netherlands"), entities and spaces
 * tidied, cut at `sizeof title - 1` bytes on a character's start. */
typedef struct {
    uint8_t st, m;
    bool    done;
    char    ent[8];
    char    title[64];
    uint8_t n;
} wsdr_title_t;
void wsdr_title_feed(wsdr_title_t *t, const uint8_t *p, size_t n);
/* Read to its end: the title, "" if none. */
const char *wsdr_title_end(wsdr_title_t *t);

/* A band plan's range (freqbands[]: kHz pairs, no names) by what a listener
 * calls it: an amateur band "40 m" (`*ham` true), a broadcast band "49 m BC",
 * "LW", "MW", "CB" -- or, none of those, its start "5.9 MHz". */
void wsdr_range_name(const wsdr_range_t *r, char *out, size_t cap, bool *ham);

/* ------------------------------------------------- which stream path */

/* The site's /websdr-sound.js, read a piece at a time: whether it opens
 * "/~~stream?v=11" (dist11) or plain "/~~stream" (Twente since 2021). */
typedef struct {
    uint8_t m;
    bool    v11;
} wsdr_v11_t;
void wsdr_v11_feed(wsdr_v11_t *s, const uint8_t *p, size_t n);
#define WSDR_STREAM      "/~~stream"
#define WSDR_STREAM_V11  "/~~stream?v=11"

/* ---------------------------------------------------------- the tuning */

/* The demodulators (§2.3). The sideband is no mode: SSB and CW are 0, which
 * side taken from the signs of lo and hi. AM sync is Twente's only. */
enum { WSDR_M_SSB = 0, WSDR_M_AM = 1, WSDR_M_AMSYNC = 2, WSDR_M_FM = 4 };

typedef struct {
    const char *name;               /* the knob's: "usb", "lsb", "cw", "am", "sam", "fm" */
    uint8_t     mode;
    int16_t     lo, hi;             /* the passband, Hz from the carrier */
} wsdr_mode_t;
/* The knob's modes, with Twente's page's passbands; NULL past the last. */
const wsdr_mode_t *wsdr_mode(int i);
const wsdr_mode_t *wsdr_mode_named(const char *name);
/* CW is heard WSDR_CW_TONE below the carrier: the station on the dial is sent
 * that much higher, its tone in the passband's middle (§2.3). */
#define WSDR_CW_TONE 750

/* What the knob tunes to: the station on the dial, Hz; the passband, Hz from
 * the carrier as the page sends it; the band. */
typedef struct {
    int64_t dial_hz;
    int     band;
    uint8_t mode;
    bool    cw;
    int32_t lo, hi;
} wsdr_tune_t;

/* The passband within the band's limit, |edge| <= 0.95 x maxlinbw (FM 15 kHz),
 * lo below hi (§2.3). */
void wsdr_clamp_pass(const wsdr_band_t *b, wsdr_tune_t *t);
/* The carrier the server is sent for the station on the dial: CW's tone
 * added, on the band's tuning step -- the server rounds to it anyway (§4.3). */
int64_t wsdr_carrier_hz(const wsdr_band_t *b, const wsdr_tune_t *t);

/* "GET /~~param?f=14074.000&band=0&lo=0.3&hi=2.7&mode=0&name=ON6URE": the
 * whole tuning, kHz as the page writes them, the name URL-encoded. Its
 * length, or -1 where it does not fit. */
int wsdr_param_cmd(char *out, size_t cap, const wsdr_band_t *b, const wsdr_tune_t *t, const char *name);
/* "GET /~~param?mute=1", "...squelch=0", "...autonotch=0". */
int wsdr_flag_cmd(char *out, size_t cap, const char *key, bool on);
/* The page sends no more than a tuning in 250 ms, the last one always. */
#define WSDR_TUNE_GAP_MS 250

#endif
