/* OpenWebRX's protocol, the parts that are plain C: a receiver's address,
 * what the knob says on the WebSocket, what the receiver says -- its JSON
 * messages read as they stream, its status.json, its audio decoded -- and the
 * rules its owners' servers hold a client to.
 *
 * OpenWebRX (jketterl's) and OpenWebRX+ (luarvique's fork) speak the same
 * protocol; where they differ it is said below. Nothing in here touches
 * ESP-IDF, so test/host builds and checks it on the PC. OWRX-PROTOCOL.md has
 * the protocol, each point with the line of the servers' source it rests on.
 *
 * A receiver is shared: its SDR is on one band at a time, for everyone
 * listening, and only a profile change (a BAND) moves it. OpenWebRX+ bans an
 * address for twelve hours after three quick ones -- so the knob never
 * changes band by itself, and never two within OWRX_SWITCH_GAP_MS. */
#ifndef OWRX_PROTO_H
#define OWRX_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------- an address */

/* A receiver's address as the page takes it: "https://host[:port]/path/",
 * "http://host[:port]/path/", or "host[:port][/path/]" in the clear. The port
 * is its scheme's (80, 443) unless it says. A receiver under a path is behind
 * a front that passes the path on (fms.komkon.org/OWRX/): the path is kept,
 * always ending in '/' -- "/OWRX" is "/OWRX/" -- with a page's name
 * ("index.html"), a query and a fragment ("#freq=") left off. */
typedef struct {
    char     host[64];
    char     path[64];              /* "/" or "/OWRX/": a '/' first and last */
    uint16_t port;
    bool     tls;
} owrx_url_t;
/* False with no host, one too long, a port outside 1..65535, a path too long
 * to keep, or a scheme other than http and https. */
bool owrx_url(const char *in, owrx_url_t *u);
/* The path under the receiver's: "<path>ws/", "<path>status.json". Its
 * length, or -1 where it does not fit. */
int owrx_path(const owrx_url_t *u, const char *leaf, char *out, size_t cap);
/* The address as the knob keeps it and shows it, read back the same by
 * owrx_url: "https://fms.komkon.org/OWRX/", "http://host:8073/" -- the port
 * only where it is not the scheme's own. Its length, or -1 where it does not
 * fit. */
int owrx_url_text(const owrx_url_t *u, char *out, size_t cap);
/* The receiver's key wherever the knob keeps something of it (never 0):
 * FNV-1a of the lower-case "host:port", as kiwi_hp() has it -- and of its
 * path after that, for one under a path, so two receivers behind one front
 * are two. */
uint32_t owrx_key(const owrx_url_t *u);
/* FNV-1a over `n` bytes, from `h` (OWRX_FNV0 to start). */
#define OWRX_FNV0 2166136261u
uint32_t owrx_fnv(uint32_t h, const void *p, size_t n);

/* ------------------------------------------------------ what it says */

/* The JSON reader under the messages and status.json: fed a message a piece
 * at a time, wherever the frames and the reads break it, it keeps only the
 * path to where it is -- each level's key, or that it is in an array -- and
 * the scalar being read. Its own; nothing outside owrx_proto.c reads it. */
#define OWRX_JDEPTH 8               /* levels with their keys kept: to 64, the rest counted */
#define OWRX_JKEY   24
#define OWRX_JSTR   128             /* a string's first bytes; its hash covers all of it */
typedef struct {
    uint8_t  st;                    /* what comes next */
    uint8_t  depth;                 /* levels open: 64 at most */
    uint8_t  esc;                   /* in a string: after '\', or the \u's digits */
    bool     bad;                   /* not JSON: the rest is not read */
    bool     cut;                   /* the string was longer than s holds */
    uint16_t u, hi;                 /* a \u's code unit; a high surrogate waiting */
    uint16_t n;                     /* bytes in s */
    uint32_t h;                     /* the string's FNV-1a, all of it */
    uint64_t arr;                   /* bit l: level l is an array */
    char     key[OWRX_JDEPTH][OWRX_JKEY];
    char     s[OWRX_JSTR];
} owrx_json_t;

/* What a text frame changed (owrx_text_end): OWRX_EV_*, ORed. */
#define OWRX_EV_HELLO   0x0001      /* "CLIENT DE SERVER": its version in `version` */
#define OWRX_EV_SPAN    0x0002      /* where it listens: retune, then start again */
#define OWRX_EV_SDR     0x0004      /* ...on another SDR: its audio starts over */
#define OWRX_EV_CONFIG  0x0008      /* something else of its config */
#define OWRX_EV_METER   0x0010
#define OWRX_EV_BANDS   0x0020      /* its profiles, the BAND list, anew */
#define OWRX_EV_NAME    0x0040      /* receiver_details */
#define OWRX_EV_CLIENTS 0x0080
#define OWRX_EV_LOG     0x0100      /* log_message: `log` */
#define OWRX_EV_DEMOD   0x0200      /* demodulator_error: `log` */
#define OWRX_EV_END     0x0400      /* backoff, sdr_error, or not OpenWebRX: `end` */

/* How a session ended, or why none could start. */
typedef enum {
    OWRX_END_NONE,          /* going on */
    OWRX_END_WANT,          /* the knob ended it: another receiver, a new list */
    OWRX_END_NOT_FOUND,     /* the name looked up to nothing */
    OWRX_END_NO_ROUTE,      /* no connection: refused, unreachable, timed out */
    OWRX_END_NO_SOCKET,     /* none free on the knob */
    OWRX_END_CERT,          /* over TLS: its certificate does not verify */
    OWRX_END_NO_ANSWER,     /* no "CLIENT DE SERVER" -- or nothing after it */
    OWRX_END_CLOSED,        /* closed while playing */
    OWRX_END_QUIET,         /* nothing at all for 10 s */
    OWRX_END_PROTOCOL,      /* something no OpenWebRX says */
    OWRX_END_NOT_OWRX,      /* another server: HTTP 400 or 404, another hello */
    OWRX_END_REFUSED,       /* HTTP 401, 403: its front's */
    OWRX_END_DOWN,          /* HTTP 5xx: its front's, OpenWebRX not behind it */
    OWRX_END_MOVED,         /* a redirect not followed */
    OWRX_END_FULL,          /* backoff: "Too many clients" -- or a reason it did not give */
    OWRX_END_BANNED,        /* backoff: "Client address banned" (OpenWebRX+) */
    OWRX_END_NO_SDR,        /* sdr_error: none of its SDRs runs */
} owrx_end_t;
#define OWRX_END_LAST OWRX_END_NO_SDR

/* The face's word for it, at most 15 characters ("" for NONE and WANT). */
const char *owrx_end_word(owrx_end_t e);
/* The upgrade's answer by its HTTP status: 101 is in (NONE); 401 and 403
 * REFUSED; 400 and 404 NOT_OWRX; 429 FULL; 5xx DOWN; a redirect MOVED (one
 * that is not followed); 0 (no status line) NO_ANSWER; anything else
 * NOT_OWRX. */
owrx_end_t owrx_http(int status);
/* How long before trying a receiver again after `e`, the `tries`-th time in a
 * row (from 1), in ms, before jitter: as the receivers' own page waits --
 * 32 s after FULL; 1, 2, 4 ... 512 s after the rest; NO_SDR from 60 s. 0: not
 * by itself -- BANNED, CERT, MOVED, NOT_OWRX, REFUSED, until it is chosen
 * again. */
uint32_t owrx_retry_ms(owrx_end_t e, unsigned tries);

/* A band: one of the receiver's profiles, "<sdr>|<profile>" and its name
 * "<sdr name> <profile name>" -- and, from status.json, where it is. */
#define OWRX_BANDS   24             /* kept; more are counted (bands_seen) */
#define OWRX_ID_MAX  80             /* "uuid|uuid" is 73 */
#define OWRX_NAME_MAX 48
typedef struct {
    char     id[OWRX_ID_MAX];       /* "" where it was too long to send back */
    char     name[OWRX_NAME_MAX];   /* cut where it must be; never mid-character */
    uint32_t name_h;                /* owrx_fnv() of the whole name, for the join */
    uint8_t  label;                 /* where its own name starts in `name`: past the SDR's */
    int64_t  center;                /* status.json's: 0 not known */
    int32_t  rate;
} owrx_band_t;

/* What one session has been told. */
typedef struct {
    /* the hello */
    bool     hello;
    char     version[24];           /* "v1.2.126" */
    /* config, merged: a diff each time, nulls ignored */
    int64_t  center, start;         /* Hz; 0 not said */
    int32_t  rate;                  /* samp_rate: the span is center +- rate/2 */
    char     start_mod[16];
    char     sdr_id[40], profile_id[40];
    int16_t  sq_init;               /* initial_squelch_level, dB; -150 where not an integer */
    float    wf_min, wf_max;        /* waterfall_levels, dB: the meter's scale */
    bool     audio_raw;             /* audio_compression "none": int16, not ADPCM */
    bool     fft_raw;               /* fft_compression "none": 147 kB/s of waterfall */
    int32_t  fft_size, max_clients, tuning_step;
    bool     plus;                  /* OpenWebRX+: one of its own keys or messages */
    /* the rest */
    float    meter;                 /* smeter: linear power, as it says it */
    bool     have_meter;
    int      clients;               /* every listener on the receiver, -1 not said */
    char     name[48];              /* receiver_details' receiver_name */
    char     log[64];               /* the last log_message or demodulator_error */
    owrx_end_t end;
    char     reason[48];            /* backoff's reason, sdr_error's text */
    owrx_band_t bands[OWRX_BANDS];
    uint8_t  n_bands;
    uint16_t bands_seen;            /* in its last profiles message, kept or not */
    /* the frame being read */
    owrx_json_t j;
    uint32_t ev;
    uint8_t  kind, mode;            /* its type; JSON, the hello line, or not yet known */
    bool     skip;                  /* a type the knob has no use for: not read */
    char     type[24];              /* its type, as it said it: for the log */
    char     line[96];
    uint8_t  line_n;
    uint8_t  nb;                    /* bands read so far, in a profiles message */
    uint16_t seen;                  /* ...and seen, kept or not */
    owrx_band_t b;                  /* the one being read */
    bool     b_id;                  /* ...its id said */
    char     sdr_was[40];           /* sdr_id before this config */
} owrx_said_t;

void owrx_said_init(owrx_said_t *s);
/* A text frame, a piece at a time: begin, feed, end. What it changed comes
 * back from owrx_text_end (OWRX_EV_*). A type the knob has no use for --
 * bookmarks, hundreds of kB of them on OpenWebRX+, modes, features -- is
 * known by its first bytes and the rest passed over unread. */
void     owrx_text_begin(owrx_said_t *s);
void     owrx_text_feed(owrx_said_t *s, const uint8_t *p, size_t n);
uint32_t owrx_text_end(owrx_said_t *s);

/* Whether the span is known, and the dial `hz` is within it: center +-
 * rate/2. */
bool owrx_in_span(const owrx_said_t *s, int64_t hz);
/* The band it listens on, by its id: an index into bands[], or -1. */
int  owrx_band_now(const owrx_said_t *s);
/* The first band that holds `hz`, as status.json has them; -1 none. */
int  owrx_band_for(const owrx_said_t *s, int64_t hz);

/* The S-meter as the receiver's page shows it: 10 log10 of the power, dB
 * against its own full scale (no dBm: neither fork has one), -150 for
 * nothing. */
float owrx_db(float power);
/* ...and the scale its page draws it on: waterfall_levels' min - 20 to
 * max + 20 (-108..0 as they come). */
void  owrx_meter_range(const owrx_said_t *s, float *lo, float *hi);
/* The squelch, 0-100 % (0 open) as the knob has it, as the receiver's dB:
 * -150, open, for 0; else that much of the way up the meter's scale -- where
 * the face draws its mark. */
int16_t owrx_squelch_db(const owrx_said_t *s, uint8_t pct);

/* ---------------------------------------------------- status.json */

/* A receiver's status.json: its name and version, and each SDR's profiles
 * with where they are -- what the BAND list shows beside each name. The
 * WebSocket's profiles have no frequencies; this has no ids. They are joined
 * by name ("<sdr name> <profile name>"), else by their order in each SDR. */
#define OWRX_STATUS_MAX 64
typedef struct {
    uint32_t h;                     /* owrx_fnv() of "<sdr name> <profile name>"; 0 unknown */
    int64_t  center;
    int32_t  rate;
    uint8_t  sdr, k;                /* which SDR, which of its profiles */
    uint8_t  sdr_len;               /* the SDR's name's length, with its space */
} owrx_sprof_t;
typedef struct {
    bool     ok;                    /* OpenWebRX's: "sdrs" said */
    char     name[48];
    char     version[24];
    int      max_clients;
    uint8_t  n_sdrs;
    uint16_t profiles;              /* all of them, kept or not */
    owrx_sprof_t p[OWRX_STATUS_MAX];
    uint8_t  n;
    /* what is being read */
    owrx_json_t j;
    char     sdr[64];               /* the SDR's name; "" not yet, or too long */
    bool     sdr_named;
    uint8_t  k;
    owrx_sprof_t cur;
    char     pname[64];
    bool     pnamed;
} owrx_status_t;
void owrx_status_init(owrx_status_t *st);
void owrx_status_feed(owrx_status_t *st, const uint8_t *p, size_t n);
bool owrx_status_end(owrx_status_t *st);
/* Its bands given where they are, and their own names (label) -- matched
 * by name, else by order where every SDR's count agrees. How many it placed. */
int  owrx_join(owrx_said_t *s, const owrx_status_t *st);

/* OpenWebRX+ by its version alone (status.json, the hello): 1.2.3 and up of
 * 1.2 -- upstream's last 1.2 is 1.2.2, and it went on to 1.3. */
bool owrx_plus_version(const char *v);

/* -------------------------------------------------------- what it says */

/* The modes the knob plays, by OpenWebRX's names, and the passband each
 * opens with -- around the DIAL, as the face, the radio page and the API all
 * have it. SAM is OpenWebRX+'s alone (upstream's audio stops on it); USB's
 * and LSB's passbands differ between the two. In CW the dial is the carrier
 * and the receiver's passband is OWRX_CW_TONE above it: the station on the
 * dial is heard at 800 Hz, as on OpenWebRX+'s page. */
#define OWRX_CW_TONE 800
#define OWRX_M_PLUS  0x01           /* OpenWebRX+ only */
#define OWRX_M_HD    0x02           /* its audio at 48 kHz (frame 0x04) */
typedef struct {
    const char *mod;
    int32_t lo, hi;                 /* upstream's, around the dial */
    int32_t plo, phi;               /* OpenWebRX+'s */
    uint8_t flags;
} owrx_mode_t;
const owrx_mode_t *owrx_mode_find(const char *mod);
/* The mode the knob plays for any name -- a profile's start_mod, another
 * radio's mode: its own where the table has it (SAM to AM upstream); a
 * digital mode's underlying one (ft8 on USB, packet on NFM, DMR on NFM); and
 * for one with none, by the frequency -- LSB below 10 MHz, USB to 30, NFM
 * above. Never NULL. */
const char *owrx_mode_of(const char *any, int64_t hz, bool plus);
/* The passband a mode opens with, around the dial, by fork. */
void owrx_passband(const owrx_mode_t *m, bool plus, int32_t *lo, int32_t *hi);

/* Where to listen: the dial, its mode, the passband around the dial, the
 * squelch in the receiver's dB (-150 open). */
typedef struct { int64_t hz; char mod[8]; int32_t lo, hi; int16_t sq; } owrx_tune_t;

/* The dial onto the receiver's span, as its own page keeps it: within the
 * span it stays; outside, the band's own start (start_freq, else its
 * centre) in the band's own mode -- a digital one's underlying -- with that
 * mode's passband. And a mode this receiver cannot play (SAM upstream, a
 * name not its own) becomes the nearest it can. True when anything moved. */
bool owrx_retune(const owrx_said_t *s, owrx_tune_t *t);

/* What the knob says. Each writes the message and gives its length, or -1
 * where it does not fit -- or, for owrx_tune_cmd, a mode it cannot send. */
#define OWRX_RATE    12000          /* output_rate: what every browser asks for */
#define OWRX_HD_RATE 48000          /* hd_output_rate: WFM's */
/* "SERVER DE CLIENT client=VFO-Knob type=receiver": a text frame, first. */
int owrx_hello_cmd(char *out, size_t cap);
/* connectionproperties: OWRX_RATE and OWRX_HD_RATE. Nothing else -- a key a
 * fork does not know is kept and fails again at every restart of its SDR,
 * for everyone on it. */
int owrx_props_cmd(char *out, size_t cap);
/* dspcontrol's params: the keys in `what` (OWRX_P_*), in the order the
 * browser sends them, the passband moved to the receiver's frame (CW's tone)
 * and kept within what its page allows, the offset an integer within the
 * span. A mode the table has not, or SAM to upstream, is -1. */
#define OWRX_P_PASS   0x01          /* low_cut, high_cut */
#define OWRX_P_OFFSET 0x02          /* offset_freq */
#define OWRX_P_MOD    0x04          /* mod */
#define OWRX_P_SQ     0x08          /* squelch_level */
#define OWRX_P_PLAIN  0x10          /* secondary_mod false: no digital decoder over it */
#define OWRX_P_ALL    0x1F
int owrx_tune_cmd(char *out, size_t cap, const owrx_tune_t *t, int64_t center, int32_t rate,
                  bool plus, unsigned what);
/* {"type":"dspcontrol","action":"start"}: nothing plays until it is said --
 * and again after any change of where it listens (OWRX_EV_SPAN). */
int owrx_start_cmd(char *out, size_t cap);
/* selectprofile: another band, for everyone on the receiver's SDR. Never
 * within OWRX_SWITCH_GAP_MS of the last, or of the session's start. */
int owrx_select_cmd(char *out, size_t cap, const char *id);
/* OpenWebRX+ adds 10 - (seconds since the last change, or since the
 * session started) for each band change and bans the address at 30; a gap
 * over 10 s clears the count. */
#define OWRX_SWITCH_GAP_MS 11000

/* ----------------------------------------------------------- audio */

/* A binary frame's first byte. Every other kind is read and let go: the
 * waterfall cannot be declined, and a client 11 s behind on it is closed. */
#define OWRX_BIN_FFT   0x01
#define OWRX_BIN_AUDIO 0x02         /* at OWRX_RATE */
#define OWRX_BIN_FFT2  0x03         /* a digital decoder's */
#define OWRX_BIN_HD    0x04         /* at OWRX_HD_RATE */

/* The audio: IMA-ADPCM, two samples a byte, the low nibble first, its
 * encoder's state given before every 1001 bytes -- "SYNC", the step index
 * and the predictor, int16 little-endian -- anywhere in a frame, or across
 * two. One decoder for both kinds of audio frame, as the receiver has one
 * encoder for both. OpenWebRX 1.0 and 1.1 send the same codec without the
 * SYNCs, from index 0 and predictor 0 (csdr's encode_ima_adpcm_i16_u8): a
 * session's audio not starting with "SYNC" is taken as that, plain. Or,
 * with audio_compression "none", int16 little-endian, a sample split across
 * two pieces carried over. */
typedef struct {
    int32_t  pred;
    int16_t  idx;
    uint8_t  st, match, hn;
    uint8_t  hdr[4];
    uint16_t left;                  /* data bytes until the next SYNC */
    uint16_t watch;                 /* plain: bytes left to look for a SYNC in still */
    bool     plain;                 /* no SYNCs: OpenWebRX 1.0 and 1.1 */
    bool     odd;                   /* int16: a low byte waiting */
    uint8_t  lo;
    uint32_t syncs, lost;           /* SYNCs taken; bytes passed over looking for one */
} owrx_adpcm_t;
/* From nothing: a new session, its audio's kind not known yet. */
void   owrx_adpcm_reset(owrx_adpcm_t *d);
/* Another SDR's encoder, or a new DSP, in the same session: looking for its
 * SYNC -- or, plain, on as before, as those receivers' own page goes on. */
void   owrx_adpcm_resync(owrx_adpcm_t *d);
/* `n` bytes of ADPCM in, at most 2n samples out: how many. */
size_t owrx_adpcm_feed(owrx_adpcm_t *d, const uint8_t *in, size_t n, int16_t *out);
/* `n` bytes of int16 in, at most n/2 + 1 samples out: how many. */
size_t owrx_pcm_feed(owrx_adpcm_t *d, const uint8_t *in, size_t n, int16_t *out);

#endif /* OWRX_PROTO_H */
