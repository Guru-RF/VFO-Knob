/* KiwiSDR's protocol, the parts that are plain C: what a receiver's /status
 * says, the rule that decides when a day-limit mark may lift, and what a
 * session hears and says -- its MSGs folded into what they mean, its SND
 * frames, the tune command, its audio decoded and brought to the knob's rate.
 *
 * A KiwiSDR and a Web-888 (RaspSDR's port of the KiwiSDR server) speak the
 * same protocol, and an UberSDR's Kiwi input imitates it. Nothing in here
 * touches ESP-IDF, so test/host builds and checks it on the PC; kiwi_sess.h
 * is the part with sockets.
 *
 * The owners' limits are why this exists first. A Kiwi bars an address for
 * good after five logins it refused for its day limit -- every browser behind
 * that address with it -- so the knob keeps its own count of such refusals
 * per receiver (kiwi_mark.h) and lifts it only on the receiver's own word.
 */
#ifndef KIWI_PROTO_H
#define KIWI_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The knob's own refused logins per mark, at most: the receiver's fifth bars
 * the address, and this leaves the household three. */
#define KIWI_STRIKES_MAX 2

enum { KIWI_KIND_UNKNOWN, KIWI_KIND_KIWISDR, KIWI_KIND_WEB888 };

/* A receiver's /status, line by line ("key=value"). -1 or 0: not said. */
typedef struct {
    bool     ok;                     /* a Kiwi's: it said sdr_hw= */
    char     name[64], antenna[48], loc[48], sw[32];
    int      users, users_max, ext_api;     /* ext_api: KiwiSDR only */
    int64_t  bands_lo, bands_hi;            /* the owner's coverage, not a limit */
    double   offset_khz;
    uint8_t  kind;                          /* KIWI_KIND_*, from sw_version */
    bool     tlimits;                       /* sdr_hw shows the hourglass: time limits */
    bool     offline;
    uint32_t date_utc, uptime_s;            /* its clock, and seconds since it started */
    bool     timed;                         /* both said, and its clock set: date - uptime
                                               is when it started */
} kiwi_status_t;

/* The body (the headers may come too: no key matches them). The text is cut
 * up in place. */
void kiwi_status_parse(char *text, kiwi_status_t *out);

/* asctime's "Thu Oct  2 13:12:00 2026", UTC, into seconds since 1970. */
bool kiwi_asctime(const char *s, uint32_t *utc);

/* FNV-1a of the lower-case "host:port": a receiver's key wherever the knob
 * keeps something of it (never 0, which means none). `port` is the one it is
 * known by: its own -- or, where an https:// redirect moved it, the one it
 * had before, so that what the knob keeps of it stays its (kiwi_sess.h's
 * kport). */
uint32_t kiwi_hp(const char *host, uint16_t port);

/* A receiver's address as the page takes it, or a redirect's Location:
 * "https://host[:port][/...]", TLS on 443 unless it says; "http://host[:port]
 * [/...]", in the clear on 80; or "host[:port]", in the clear on `dflt` -- a
 * Kiwi's own 8073 on the page; 0 where a scheme is wanted (a Location), and
 * false without one. Spaces before it left out. False too with no host, one
 * longer than `cap` - 1, or a port outside 1..65535. */
bool kiwi_url(const char *in, uint16_t dflt, char *host, size_t cap, uint16_t *port, bool *tls);

/* The Host a request names the receiver by, as a browser does: "host", or
 * "host:port" where the port is not its scheme's own (80 in the clear, 443
 * over TLS). A front -- Cloudflare's, the kiwisdr.com proxy -- routes on it. */
void kiwi_host_hdr(char *out, size_t cap, const char *host, uint16_t port, bool tls);

/* The value of header `name` (case aside) in an HTTP answer's head -- its
 * lines up to the blank one, CR LF or LF apart, the status line first --
 * into `v`, spaces at either end left off, cut at cap - 1. False: none. */
bool kiwi_http_header(const char *head, const char *name, char *v, size_t cap);

/* Where a receiver's answer sent the knob. An http:// receiver's 301, 302,
 * 307 or 308 to https:// on its own host is followed there and then, once,
 * in TLS, and the caller keeps https for it from then on: `tls_port`, the
 * port that speaks it (0: none followed). Any other redirect is not -- one
 * to another host or in the clear, one from a receiver already spoken over
 * TLS, a second: `to` says where it pointed, for "moved to <to>" ("": none). */
typedef struct { uint16_t tls_port; char to[64]; } kiwi_moved_t;

/* A redirect's status: 301, 302, 307 or 308 -- the ones followed. */
bool kiwi_redirect_code(int status);

/* Whether to follow a redirect from the receiver at `host`, spoken over TLS
 * or not, to `loc` (its Location): only https:// on that same host, from a
 * receiver spoken in the clear -- *port then where TLS waits (443 unless it
 * says). Not followed, `to` gets where it points: its host -- one too long
 * to follow, or after "//" with no scheme, as much as `to` holds -- or the
 * receiver's own for a path alone or an http:// one on its host; "" for no
 * Location at all. */
bool kiwi_redirect(const char *loc, const char *host, bool tls, uint16_t *port, char *to, size_t cap);

/* %XX undone, in place: what a receiver says in a MSG. */
void kiwi_unescape(char *s);

/* One receiver's day-limit mark, 16 bytes as NVS keeps it ("kdl"). */
typedef struct {
    uint32_t hp;            /* kiwi_hp() */
    uint8_t  strikes;       /* the knob's own logins it refused -- or may have: never fewer */
    uint8_t  kind;          /* KIWI_KIND_*, once known */
    uint8_t  flags;         /* KIWI_MARK_* */
    uint8_t  seq;           /* how recent, mod 256: which goes when the table is full */
    uint32_t rx_boot;       /* when the receiver had started, its clock; 0 unknown */
    uint32_t mark_utc;      /* its clock at the last refusal; 0 unknown */
} kiwi_mark_t;

/* Its /status has shown the hourglass since the mark: no hourglass now means
 * its owner took the time limits away. Without it, "no hourglass" is what
 * the receiver said when it refused the knob anyway -- proof of nothing. */
#define KIWI_MARK_HG     0x01
/* No ip_limit was seen: a login it left unanswered, counted in case it was
 * refused, as the knob cannot tell. */
#define KIWI_MARK_UNSURE 0x02
/* It has played since: it holds nothing back, but its strikes are kept, as
 * the receiver keeps its own count until it restarts -- or, a KiwiSDR, for
 * a day. */
#define KIWI_MARK_REST   0x04

/* Whether a fresh /status proves the receiver's count of refused logins was
 * cleared since the mark: it has restarted (started more than two minutes
 * later than it had), or it is a KiwiSDR a day past the last refusal (it
 * clears the count once a day; a Web-888 only when it restarts). Then the
 * mark can go, strikes and all. Unknown times prove nothing. */
bool kiwi_mark_cleared(const kiwi_mark_t *m, const kiwi_status_t *st);

/* Whether a fresh /status lets the mark stop holding the receiver back: its
 * count cleared (kiwi_mark_cleared), or its owner has taken the time limits
 * away -- the hourglass, seen before, gone. That proves nothing of its count,
 * which a Web-888 keeps until it restarts, limits or none: the mark then
 * rests, its strikes kept (kiwi_mark_check). */
bool kiwi_mark_lifted(const kiwi_mark_t *m, const kiwi_status_t *st);

/* ------------------------------------------------------------ a session */

#define KIWI_OUT_HZ    24000            /* the knob's audio rate (AUDIO_RATE_HZ) */
#define KIWI_PCM_MAX   4096             /* one SND frame, decoded (ADPCM: 2048) */
#define KIWI_OUT_MAX   (KIWI_PCM_MAX * 2 + 64)
/* A KiwiSDR's CW tone, its CW passband's centre (300..700): carrier = dial -
 * 500 -- until its load_cfg says where it centres CW (kiwi_cw_feed). */
#define KIWI_CW_PITCH  500

/* Its modes, and the passband each opens with -- relative to the DIAL, as the
 * face, the radio page and the API all have it. */
typedef struct { const char *m; int16_t lo, hi; } kiwi_mode_t;
/* A mode by the Kiwi's own name ("usb", "sam", "nbfm" ... and the narrow
 * "usn", "lsn", "amn", "cwn", "nnfm"); NULL for any other, and for the ones
 * the knob never asks for (iq, drm, sas, qam: stereo; amw). */
const kiwi_mode_t *kiwi_mode_find(const char *m);
/* Any radio's mode name as the Kiwi's: cw/cwr/cwu/cwl -> cw, digl -> lsb,
 * fm/nfm/wfm -> nbfm, digu, rtty and the rest -> usb; the Kiwi's own pass. */
const char *kiwi_mode_of(const char *any);
/* The sideband a voice takes on an IARU Region 1 band: "lsb" below 10 MHz,
 * "usb" above and on 60 m; NULL off the bands. */
const char *kiwi_sideband(int64_t hz);

/* Where to listen: the dial, its mode, the passband around the dial. */
typedef struct { int64_t hz; char mode[6]; int32_t lo, hi; } kiwi_tune_t;
/* "SET mod=<m> low_cut=<lo> high_cut=<hi> freq=<kHz>" for the receiver: in CW
 * the carrier `cw` below the dial and the passband around that tone, as the
 * Kiwi's own page tunes (freq_dsp_to_car) -- `cw` where the receiver centres
 * CW: a KiwiSDR's KIWI_CW_PITCH, or what its load_cfg says; 0 on an UberSDR's
 * Kiwi input, which centres CW on the carrier and makes the tone itself. The
 * carrier kept within offset..offset+bandwidth (bw_hz 0: not said) and sent
 * as the baseband it is, kHz less the offset; the passband within the audio
 * rate's +-rate/2 (0: 12 kHz). Its length, or -1. */
int kiwi_tune_cmd(char *out, size_t cap, const kiwi_tune_t *t, double offset_khz, int64_t bw_hz,
                  double rate, int32_t cw);
/* Whether the receiver reaches the dial: within offset..offset+bandwidth,
 * what it says it covers (bw_hz 0: not said, anything over the offset).
 * Outside, kiwi_tune_cmd keeps the carrier at the edge, and the receiver
 * would play that instead. A CW dial less than its tone over the offset is
 * reached all the same, its carrier kept at the offset: the station there
 * heard below its tone, as on the kiwi firmware's own dial, which is held
 * within that range. */
bool kiwi_tune_reaches(const kiwi_tune_t *t, double offset_khz, int64_t bw_hz);

/* Where a receiver centres its CW passband -- the tone a station on the dial
 * is heard at -- as its configuration says: "MSG load_cfg=", JSON URL-encoded,
 * "passbands":{.."cw":{"lo":300,"hi":700}..}. A KiwiSDR's as its owner set it
 * (300..700 as it comes: 500); an UberSDR's Kiwi input's -400..400 (0). Read
 * as it comes, wherever the frames and the reads break it: kiwi_cw_feed() is
 * given the frame's text from its start, "MSG ", a piece at a time -- any
 * other MSG is read to nothing. The first "cw" object counts. */
typedef struct {
    char    obj[48];                /* the object's text, as far as it came */
    uint8_t st;                     /* where it is: the prefix, the key, the object, read */
    uint8_t at;                     /* how much of "MSG load_cfg=", then of "cw":{, has come */
    uint8_t n;                      /* how much of obj is filled */
    uint8_t esc;                    /* a %XX broken off: how much of it has come (1, 2) */
    char    dig;                    /* ...its first digit */
} kiwi_cw_t;
void kiwi_cw_init(kiwi_cw_t *c);
void kiwi_cw_feed(kiwi_cw_t *c, const uint8_t *p, size_t n);
/* (lo + hi) / 2 where its "cw" object said lo < hi, and that within
 * -1000..1500 Hz; else `dflt`, as before it said anything. */
int32_t kiwi_cw_centre(const kiwi_cw_t *c, int32_t dflt);

/* The AGC: KIWI_AGC_* -- the decay the Kiwi's own presets have, and in CW
 * the threshold its page takes there. */
enum { KIWI_AGC_FAST, KIWI_AGC_MED, KIWI_AGC_SLOW };
int kiwi_agc_cmd(char *out, size_t cap, uint8_t agc, bool cw);
/* The squelch, 0-100 % (0 open): NBFM's own scale, else dB over the noise. */
int kiwi_squelch_cmd(char *out, size_t cap, uint8_t pct, bool nbfm);
/* The noise filter, KIWI_NR_*: its commands one by one, the i-th from 0 --
 * false past the last. The pages' noise_filter.js sends just these. */
enum { KIWI_NR_OFF, KIWI_NR_WDSP, KIWI_NR_LMS, KIWI_NR_SPEC };
bool kiwi_nr_cmd(char *out, size_t cap, uint8_t nr, int i);

/* Who the receiver's owner sees of the knob in its list of listeners: "SET
 * ident_user=<who>", the identity KiwiSDR's protocol gives each one -- the
 * operator's callsign, say; "VFO-Knob" for NULL or "". URL-encoded as the
 * Kiwi's own page sends it (encodeURIComponent): the receiver reads the
 * command up to a space. KIWI_IDENT_MAX bounds what is kept of it. Its
 * length, or -1. */
#define KIWI_IDENT_MAX 32
#define KIWI_IDENT_DEFAULT "VFO-Knob"
int kiwi_ident_cmd(char *out, size_t cap, const char *who);

/* The value of `key` in a MSG's "key=value key2=value" list, into v; a key
 * said alone, without a value, gives "". */
bool kiwi_msg_val(const char *msg, const char *key, char *v, size_t cap);

/* How a session ended, or why none could start. */
typedef enum {
    KIWI_END_NONE,          /* going on */
    KIWI_END_WANT,          /* the knob ended it: another receiver, a new list */
    KIWI_END_NOT_FOUND,     /* the name looked up to nothing */
    KIWI_END_NO_ROUTE,
    KIWI_END_NO_SOCKET,     /* none free on the knob */
    KIWI_END_NO_ANSWER,     /* nothing, or a close before a word */
    KIWI_END_SILENT,        /* a KiwiSDR's door for apps: no word, the socket kept */
    KIWI_END_CLOSED,        /* closed after the login */
    KIWI_END_QUIET,         /* nothing for 10 s */
    KIWI_END_PROTOCOL,      /* something no Kiwi says */
    KIWI_END_REFUSED,       /* not from this address (badp=3, HTTP 403) */
    KIWI_END_NOT_KIWI,      /* HTTP 400, 404 */
    KIWI_END_PASSWORD,
    KIWI_END_PWD_FULL,      /* every channel without a password taken */
    KIWI_END_DUP_IP,        /* one connection per address */
    KIWI_END_UPDATING,
    KIWI_END_TRY_LATER,
    KIWI_END_FULL,
    KIWI_END_APPS_FULL,     /* the channels it keeps for apps, taken */
    KIWI_END_NO_APPS,       /* none for apps at all */
    KIWI_END_DAY_LIMIT,     /* ip_limit: the day's listening used up */
    KIWI_END_IDLE,          /* inactivity_timeout: time up */
    KIWI_END_KICKED,        /* kiwi_kick: sent away by its owner */
    KIWI_END_DOWN,
    KIWI_END_NO_FLASH,      /* the knob's own: its flash cannot keep a day-limit mark */
    KIWI_END_MOVED,         /* a redirect not followed: elsewhere, or a second (kiwi_moved_t) */
    KIWI_END_CERT,          /* over TLS: its certificate does not verify -- not spoken to */
} kiwi_end_t;
#define KIWI_END_LAST KIWI_END_CERT

/* The upgrade's answer, by its HTTP status: 101 is in (NONE); 401/403
 * REFUSED; 429 and 503 FULL; another 5xx DOWN; a redirect (301, 302, 303,
 * 307, 308) MOVED -- one that is not followed; anything else NOT_KIWI; 0 (no
 * status line) NO_ANSWER. */
kiwi_end_t kiwi_http(int status);

/* What one session has been told. -1 (or 0) where it has not said. */
typedef struct {
    int     rx_chans, chan_no_pwd, badp, too_busy, version_maj, version_min;
    int     audio_rate;                 /* its audio_rate= */
    double  rate;                       /* its sample_rate=: the audio's exact rate */
    double  offset_khz;                 /* freq_offset= */
    int64_t center_hz, bw_hz;           /* center_freq=, bandwidth= */
    bool    logged_in, said_anything;
    bool    limit_at_login;             /* its ip_limit refused the login: counted */
    char    kick[48];                   /* kiwi_kick's message, decoded */
    /* The session's own (kiwi_sess.c): it reached the receiver -- a
     * connection made, whatever came of it -- and its login went out and
     * had no answer before the session ended, though the receiver did not
     * close it either: it may have refused it, and counted that. */
    bool    connected, unanswered;
    /* ...and where its load_cfg says it centres CW (kiwi_cw_centre):
     * KIWI_CW_PITCH until it says. */
    int32_t cw_hz;
    /* ...and where its answer to the upgrade sent the knob: followed, to
     * https://, or not (KIWI_END_MOVED). */
    kiwi_moved_t moved;
} kiwi_said_t;
void kiwi_said_init(kiwi_said_t *s);
/* One MSG's body (after "MSG "), folded in: how it ends the session, or
 * NONE. `pass_given`: the login carried a password. */
kiwi_end_t kiwi_said(kiwi_said_t *s, const char *msg, bool pass_given);
/* No answer to the login: what that means. A KiwiSDR (or a receiver not yet
 * known) saying nothing at all with the socket open has shut its door for
 * apps -- SILENT, or NO_APPS where its /status says it lets none (ext_api 0);
 * a Web-888 answers or closes, so its silence is NO_ANSWER. A close before
 * any word is NO_ANSWER, one after the login CLOSED. */
kiwi_end_t kiwi_quiet(const kiwi_said_t *s, uint8_t kind, int ext_api, bool closed);
/* The face's word for it (link_why: at most 15 characters, 264 px at 28 pt;
 * "" for NONE and WANT), and the second receiver's (at most 11). */
const char *kiwi_end_word(kiwi_end_t e);
const char *kiwi_end_note(kiwi_end_t e);

/* An SND frame: flags, sequence, S-meter, and where its audio starts. */
typedef struct { uint8_t flags; uint32_t seq; uint16_t smeter; size_t off; } kiwi_snd_t;
#define KIWI_SND_ADC_OVFL 0x02
#define KIWI_SND_NEW_FREQ 0x04
#define KIWI_SND_STEREO   0x08          /* I/Q, 20 bytes of header: never asked for */
#define KIWI_SND_ADPCM    0x10
#define KIWI_SND_SQUELCH  0x40          /* closed: the audio still comes, to be silenced */
#define KIWI_SND_LE       0x80          /* PCM little-endian */
bool  kiwi_snd(const uint8_t *p, size_t n, kiwi_snd_t *out);       /* p at "SND" */
/* The S-meter in dBm: 0.1 dB steps over -127 dBm -- over -140 on a Web-888. */
float kiwi_dbm(uint16_t raw, bool web888);

/* The audio: IMA-ADPCM decoded, then from the receiver's rate to the knob's
 * 24 kHz. Up to 26 kHz it is doubled by half-band filters until it runs at
 * 32 kHz or more -- 12 kHz twice, to 48 -- and above that it is low-passed
 * at 10.5 kHz instead; then a cubic interpolator (Catmull-Rom) takes it to
 * 24 kHz. Flat within 0.07 dB to 5 kHz at 12 kHz, and what it adds of its
 * own at least 42 dB down, whatever the drift trim. The interpolator keeps
 * its place as a 32.32 fixed point: a float's rounding, added up sample
 * after sample, ran the knob's own rate off by tens of ppm. */
#define KIWI_HB_MAX 3           /* half-band stages at most: 4 kHz to 32 */
#define KIWI_HB_ODD 16          /* each one's odd phase: 16 of its 31 taps */
#define KIWI_LP_LEN 31          /* the low-pass, over 26 kHz */
typedef struct {
    int32_t  pred;
    int      idx;
    uint8_t  hb;                            /* half-band stages at this rate */
    bool     lp;                            /* the low-pass instead: over 26 kHz */
    uint8_t  hb_at[KIWI_HB_MAX], lp_at;     /* where each history's newest is */
    float    hb_h[KIWI_HB_ODD];             /* the half-band's odd phase */
    float    hb_x[KIWI_HB_MAX][2 * KIWI_HB_ODD];   /* each stage's inputs, kept twice over */
    float    lp_h[KIWI_LP_LEN];
    float    lp_x[2 * KIWI_LP_LEN];
    float    y[4];                          /* the interpolator's last four inputs */
    uint64_t pos, step;     /* where it is between y[1] and y[2]; input per output (32.32) */
    float    trim;          /* drift: the step's own factor, 0.998..1.002 */
    float    rate;          /* the receiver's, Hz */
} kiwi_dsp_t;
/* A session's audio from nothing: the decoder, and the resampler at 12 kHz. */
void kiwi_dsp_init(kiwi_dsp_t *d);
/* The rate the receiver says (sample_rate=): 4000-50000 Hz, else false and
 * nothing changed -- the session ends, PROTOCOL. The resampler starts over,
 * its trim kept; the decoder goes on, as the receiver's encoder does. */
bool kiwi_dsp_reset(kiwi_dsp_t *d, double rate);
int  kiwi_adpcm(kiwi_dsp_t *d, const uint8_t *in, size_t n, int16_t *out);    /* 2n samples */
int  kiwi_pcm16(const uint8_t *in, size_t n, bool le, int16_t *out, int cap); /* n bytes */
/* `n` samples in, at most `cap` out at 24 kHz (n * 24000 / rate / trim,
 * give or take one): more would be dropped, the timing kept. */
int  kiwi_resample(kiwi_dsp_t *d, const int16_t *in, int n, int16_t *out, int cap);
/* The receiver's clock against the knob's: above 1 the step grows and its
 * audio comes out a hair shorter, so the knob's ring fills less. Clamped to
 * 0.998..1.002: 3.5 cents at most. */
void kiwi_dsp_trim(kiwi_dsp_t *d, float ratio);
/* The samples one SND frame carries (`n` bytes from "SND", `f` as kiwi_snd()
 * read it): at 24 kHz, that times 24000 / rate. */
size_t kiwi_snd_len(const kiwi_snd_t *f, size_t n);

/* One SND frame's audio (`p`, `n` from "SND"; `f` as kiwi_snd() read it), out
 * at 24 kHz through emit() a piece at a time. Every frame is decoded, heard
 * or not, so the decoder keeps in step with the receiver's encoder: with
 * `play` false (not tuned yet) it is decoded and no more, and while the
 * receiver's squelch is closed it is played as silence, its length kept.
 * `pcm` holds KIWI_PCM_MAX samples, `out` KIWI_OUT_MAX. The samples decoded:
 * 0 for a frame with no audio the knob takes (a stereo one). */
typedef void (*kiwi_emit_t)(void *ctx, const int16_t *pcm, size_t n);
int kiwi_snd_audio(kiwi_dsp_t *d, const kiwi_snd_t *f, const uint8_t *p, size_t n, bool play,
                   int16_t *pcm, int16_t *out, kiwi_emit_t emit, void *ctx);

/* A receiver's name on the dial, at most cap - 1 bytes: the one given on the
 * page; else its antenna as its /status says, "RF.Guru " and spaces at either
 * end left off; else host:port where that fits -- the host alone for port 0,
 * an https:// one on its scheme's own 443 (kiwi_label_port). Where it does
 * not, the start of its host ("kiwi1.example.n") -- unless `shared`, another
 * receiver in the list on the same host: then its end, the port with it
 * ("..83.21.23:8077"), so four receivers on one address are told apart. */
void kiwi_label(char *out, size_t cap, const char *name, const char *antenna, const char *host,
                uint16_t port, bool shared);

/* The port a receiver's name shows, as kiwi_label takes it: 0, none, for an
 * https:// one on 443 -- its address is its name alone. */
static inline uint16_t kiwi_label_port(uint16_t port, bool tls) { return tls && port == 443 ? 0 : port; }

/* The name its /status suggests for it -- the page's Test fills an empty one
 * with this: its antenna; else its own name up to a " | " (the Lombardsijde
 * receivers all say "RF.Guru Lombardsijde | ..."); "RF.Guru " and spaces at
 * either end left off. At most cap - 1 bytes, never a character cut in two;
 * "" when it says neither, or `st` is no Kiwi's. */
void kiwi_status_name(char *out, size_t cap, const kiwi_status_t *st);

#endif /* KIWI_PROTO_H */
