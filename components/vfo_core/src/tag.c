/* Reading TailChaser's callsign tag (tag.h): the message's bits, and the
 * streaming decoder that finds and reads it in microphone audio. The format's
 * numbers are tag_format.h's; what is done with them follows TailChaser's
 * reference decoder (tag2.py), made to run as the audio comes in. */
#include "tag.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* --- the message ----------------------------------------------------------------- */

static const char ALPHA[] = TAG_ALPHABET;
#define NALPHA ((int)sizeof ALPHA - 1)                       /* 39 */

static int alpha_at(char c)
{
    for (int i = 0; i < NALPHA; i++)
        if (ALPHA[i] == c) return i;
    return -1;
}

bool tag_call_ok(const char *call)
{
    const size_t n = call ? strlen(call) : 0;
    if (n == 0 || n > TAG_CALL_LEN) return false;
    for (size_t i = 0; i < n; i++)
        if (call[i] == ' ' || alpha_at(call[i]) < 0) return false;
    return true;
}

static bool has_letter(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] >= 'A' && p[i] <= 'Z') return true;
    return false;
}

static bool letters(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] < 'A' || p[i] > 'Z') return false;
    return true;
}

/* ON6URE: 1..2 with a letter or a digit and two letters (3DA0RS), a digit,
 * then 1..4 letters to the end. */
static bool call_core(const char *p, size_t n)
{
    size_t l = 0;
    while (l < n && l < 5 && p[n - 1 - l] >= 'A' && p[n - 1 - l] <= 'Z') l++;
    if (l < 1 || l > 4 || l + 2 > n) return false;
    const size_t pre = n - l - 1;                            /* p[pre] the digit */
    if (p[pre] < '0' || p[pre] > '9' || pre < 1 || pre > 3) return false;
    return pre == 3 ? p[0] >= '0' && p[0] <= '9' && letters(p + 1, 2) : has_letter(p, pre);
}

bool tag_call_likely(const char *call)
{
    if (!tag_call_ok(call)) return false;
    size_t n = strlen(call);
    const char *dash = strchr(call, '-');
    if (dash) {                                              /* ON0ORA-L: one more, at the end */
        if ((size_t)(dash - call) + 2 != n || call[n - 1] == '/') return false;
        n -= 2;
    }
    const char *slash = memchr(call, '/', n);
    if (!slash) return call_core(call, n);
    const size_t a = (size_t)(slash - call), b = n - a - 1;
    if (memchr(slash + 1, '/', b)) return false;
    /* F/ON6URE, EA8/ON6UR: a country's prefix before; ON6URE/P, /M, /MM, /AM, /QRP, /5 after */
    const char *t = slash + 1;
    const bool after = (b == 1 && ((*t >= 'A' && *t <= 'Z') || (*t >= '0' && *t <= '9'))) ||
                       (b == 2 && (!strncmp(t, "MM", 2) || !strncmp(t, "AM", 2))) || (b == 3 && !strncmp(t, "QRP", 3));
    return (a >= 1 && a <= 3 && has_letter(call, a) && call_core(slash + 1, b)) || (after && call_core(call, a));
}

size_t tag_say_text(const char *call, bool phonetic, char *out, size_t cap)
{
    static const char *const NATO[26] = { "Alfa",   "Bravo",   "Charlie", "Delta",  "Echo",    "Foxtrot", "Golf",
                                          "Hotel",  "India",   "Juliett", "Kilo",   "Lima",    "Mike",    "November",
                                          "Oscar",  "Papa",    "Quebec",  "Romeo",  "Sierra",  "Tango",   "Uniform",
                                          "Victor", "Whiskey", "X-ray",   "Yankee", "Zulu" };
    static const char *const DIGIT[10] = { "zero", "one", "two",   "three", "four",
                                           "five", "six", "seven", "eight", "nine" };
    if (!out || !cap || !tag_call_ok(call)) return 0;
    size_t n = 0;
    out[0] = 0;
    for (const char *p = call; *p; p++) {
        const char c = *p;
        char one[2] = { c, 0 };
        const char *w = c >= 'A' && c <= 'Z' ? (phonetic ? NATO[c - 'A'] : one)
                      : c >= '0' && c <= '9' ? DIGIT[c - '0']
                      : c == '/'             ? "stroke"
                                             : "dash";
        const char *sep = p == call ? "" : phonetic ? " " : ", ";
        const int k = snprintf(out + n, cap - n, "%s%s", sep, w);
        if (k < 0 || (size_t)k >= cap - n) {
            out[0] = 0;
            return 0;
        }
        n += (size_t)k;
    }
    return n;
}

uint16_t tag_crc16(const uint8_t *bits, int n)
{
    uint16_t r = TAG_CRC_INIT;
    for (int i = 0; i < n; i++) {
        r ^= (uint16_t)((bits[i] & 1) << 15);
        r = (r & 0x8000) ? (uint16_t)((r << 1) ^ TAG_CRC_POLY) : (uint16_t)(r << 1);
    }
    return r;
}

bool tag_pack(const char *call, bool local, uint8_t bits[TAG_PAYLOAD_BITS])
{
    if (!tag_call_ok(call)) return false;
    uint64_t v = 0;
    const size_t n = strlen(call);
    for (int i = 0; i < TAG_CALL_LEN; i++)
        v = v * 40 + (uint64_t)((size_t)i < n ? alpha_at(call[i]) : 0);
    for (int i = 0; i < TAG_CALL_BITS; i++) bits[i] = (uint8_t)((v >> (TAG_CALL_BITS - 1 - i)) & 1);
    bits[TAG_LOCAL_BIT] = local ? 1 : 0;
    for (int i = 0; i < TAG_ZERO_BITS; i++) bits[TAG_LOCAL_BIT + 1 + i] = 0;
    const uint16_t c = tag_crc16(bits, TAG_CRC_FROM);
    for (int i = 0; i < 16; i++) bits[TAG_CRC_FROM + i] = (uint8_t)((c >> (15 - i)) & 1);
    return true;
}

bool tag_unpack(const uint8_t bits[TAG_PAYLOAD_BITS], char call[TAG_CALL_LEN + 1], bool *local)
{
    uint16_t got = 0;
    for (int i = 0; i < 16; i++) got = (uint16_t)(got << 1 | (bits[TAG_CRC_FROM + i] & 1));
    if (got != tag_crc16(bits, TAG_CRC_FROM)) return false;
    for (int i = 0; i < TAG_ZERO_BITS; i++)
        if (bits[TAG_LOCAL_BIT + 1 + i]) return false;
    uint64_t v = 0;
    for (int i = 0; i < TAG_CALL_BITS; i++) v = v << 1 | (bits[i] & 1);
    char s[TAG_CALL_LEN + 1];
    for (int i = TAG_CALL_LEN - 1; i >= 0; i--) {
        const int c = (int)(v % 40);
        if (c >= NALPHA) return false;
        s[i] = ALPHA[c];
        v /= 40;
    }
    if (v) return false;                                     /* more than 9 characters' worth */
    s[TAG_CALL_LEN] = 0;
    int n = TAG_CALL_LEN;
    while (n > 0 && s[n - 1] == ' ') s[--n] = 0;             /* the padding */
    if (!tag_call_ok(s)) return false;                       /* empty, or a space inside */
    memcpy(call, s, (size_t)n + 1);
    if (local) *local = bits[TAG_LOCAL_BIT] & 1;
    return true;
}

static int parity(unsigned v) { return __builtin_parity(v); }

void tag_conv(const uint8_t in[TAG_PAYLOAD_BITS], uint8_t out[TAG_NBITS])
{
    unsigned sr = 0;
    for (int i = 0; i < TAG_PAYLOAD_BITS + TAG_CONV_K - 1; i++) {
        const unsigned b = i < TAG_PAYLOAD_BITS ? (in[i] & 1u) : 0u;
        sr = ((sr << 1) | b) & 0x7F;
        out[2 * i]     = (uint8_t)parity(sr & TAG_CONV_G1);
        out[2 * i + 1] = (uint8_t)parity(sr & TAG_CONV_G2);
    }
}

/* The coder's two output bits for each register value (the 6 bits before and
 * the new one): worked out once. The parity was a library call on the knob,
 * four for every state of every step, and the Viterbi most of a reading's
 * time. */
static uint8_t s_conv_out[128];
static bool    s_conv_ready;

static void conv_table(void)
{
    if (s_conv_ready) return;
    for (unsigned r = 0; r < 128; r++)
        s_conv_out[r] = (uint8_t)((parity(r & TAG_CONV_G1) << 1) | parity(r & TAG_CONV_G2));
    s_conv_ready = true;
}

void tag_viterbi(const float soft[TAG_NBITS], uint8_t out[TAG_PAYLOAD_BITS])
{
    enum { NST = 64, STEPS = TAG_NBITS / 2 };
    static const float NONE = -1e30f;
    float m[NST], nm[NST];
    uint8_t back[STEPS][NST];                                /* which predecessor: 0 or 1 */
    conv_table();
    for (int s = 0; s < NST; s++) m[s] = NONE;
    m[0] = 0;
    for (int k = 0; k < STEPS; k++) {
        const float a = soft[2 * k], b = soft[2 * k + 1];
        /* the branch metric of each output pair: 00 01 10 11 */
        const float bm[4] = { -a - b, -a + b, a - b, a + b };
        for (int s = 0; s < NST; s++) {
            const unsigned bit = (unsigned)s & 1u;
            const unsigned p0 = (unsigned)s >> 1, p1 = p0 | 32u;
            const float m0 = m[p0] + bm[s_conv_out[(p0 << 1) | bit]];
            const float m1 = m[p1] + bm[s_conv_out[(p1 << 1) | bit]];
            back[k][s] = m1 > m0;
            nm[s] = m1 > m0 ? m1 : m0;
        }
        memcpy(m, nm, sizeof m);
    }
    unsigned s = 0;                                          /* the tail bits end it in state 0 */
    uint8_t bits[STEPS];
    for (int k = STEPS - 1; k >= 0; k--) {
        bits[k] = (uint8_t)(s & 1);
        s = (s >> 1) | (back[k][s] ? 32u : 0u);
    }
    memcpy(out, bits, TAG_PAYLOAD_BITS);
}

int tag_perm(int i) { return (i * TAG_PERM_MUL) % TAG_NBITS; }

/* The frame's chips, into c (TAG_FRAME_CHIPS of them). */
static void make_chips(int8_t *c)
{
    uint32_t r = TAG_LFSR_SEED;
    for (int i = 0; i < TAG_FRAME_CHIPS; i++) {
        const uint32_t bit = (r ^ r >> 2 ^ r >> 3 ^ r >> 5) & 1u;
        r = (r >> 1) | (bit << 16);
        c[i] = (r & 1u) ? 1 : -1;
    }
}

/* One at a time, for the tests: run up to it. (No table: a static here would
 * be the knob's internal RAM.) */
int tag_chip(int i)
{
    i = ((i % TAG_FRAME_CHIPS) + TAG_FRAME_CHIPS) % TAG_FRAME_CHIPS;
    uint32_t r = TAG_LFSR_SEED;
    for (int n = 0; n <= i; n++) {
        const uint32_t bit = (r ^ r >> 2 ^ r >> 3 ^ r >> 5) & 1u;
        r = (r >> 1) | (bit << 16);
    }
    return (r & 1u) ? 1 : -1;
}

static int sync_bit(int k) { return (int)((TAG_SYNC_WORD >> k) & 1u); }

bool tag_frame_signs(const char *call, bool local, int8_t signs[TAG_NSYM])
{
    uint8_t p[TAG_PAYLOAD_BITS], c[TAG_NBITS], il[TAG_NBITS];
    if (!tag_pack(call, local, p)) return false;
    tag_conv(p, c);
    for (int i = 0; i < TAG_NBITS; i++) il[tag_perm(i)] = c[i];
    signs[0] = 1;
    for (int k = 1; k < TAG_NSYM; k++) {
        const int bit = k < TAG_NSYNC ? sync_bit(k) : il[k - TAG_NSYNC];
        signs[k] = (int8_t)(bit ? -signs[k - 1] : signs[k - 1]);
    }
    return true;
}

/* --- the decoder ------------------------------------------------------------------ */

#define DEC        4                                         /* 16 kHz in, 4 kHz complex */
#define SPC16      (TAG_FS / TAG_CHIP_RATE)                  /* 8 samples a chip at 16 kHz */
#define NS         (TAG_CPS * SPC16 / DEC)                   /* 64 samples a symbol at 4 kHz */
#define FLEN       (TAG_NSYM * NS)                           /* 10496: a frame at 4 kHz */
#define FLEN16     (FLEN * DEC)                              /* 41984 */
#define SEARCH     (TAG_NSYNC * NS)                          /* 1024: the known symbols */
#define RING       32768                                     /* ~8.2 s: three frames and more */
#define RMASK      (RING - 1)
#define NTAP       95                                        /* the decimating low-pass */
#define LP_HZ      1700.0                                    /* its middle: flat to ~1240, gone by ~2160 */
#define SHAPE_TAPS 129
#define TRY_EVERY  16                                        /* symbols between tries: 256 ms */
#define MAXSPAN    3                                         /* frames' time read at the most */
#define NFR        (MAXSPAN + 1)                             /* frames touched by that */
#define QUEUE      4
#ifndef FOLD_KEEP
#define FOLD_KEEP  0.6f                                      /* a frame's metric keeps this of those before */
#endif
#ifndef CANDS
#define CANDS      1                                         /* starts tried, the best first (more read no more) */
#endif
#define LVL_SYMS   16                                        /* a level: 256 ms */
#define FLOOR_N    40                                        /* the floor: the last ~10 s of levels */
#define DRIFT      (TAG_DRIFT_PPM * 1e-6)
#define MEMO       16                                        /* samples a symbol's nine tries read it at */

/* One symbol's despreads at the samples a reading's nine tries need it at:
 * from lo on, those in the ring (have). */
typedef struct {
    uint64_t lo;
    float    zr[MEMO], zi[MEMO];
    uint16_t have;
} memo_t;

struct tag_dec {
    /* in */
    uint64_t nin;                                            /* input samples */
    /* Single precision, as the knob's FPU has it (double is software there,
     * and per sample it cost more than all the rest of the mixing): the
     * rotator is put back exactly every NS*DEC samples, so its rounding never
     * adds up. */
    float    w_re, w_im;                                     /* the mixer's step, e^-jw */
    float    c_re, c_im;                                     /* ...and where it is */
    float    h[NTAP];                                        /* the low-pass */
    float    xr[2 * NTAP], xi[2 * NTAP];                     /* mixed input, twice over */
    int      xp;
    float    all_acc;                                        /* sum of x^2 this level */
    uint32_t dither;
    /* baseband */
    uint64_t nb;                                             /* samples at 4 kHz */
    float    br[2 * RING], bi[2 * RING];                     /* twice over: any window is in one piece */
    float    wp[RING];                                       /* |r|^2 summed over the NS samples ending here */
    float    pw[NS];                                         /* the last NS |r|^2, for that sum */
    float    pw_sum;
    /* the frame's chips as the decoder sees them */
    int8_t   chips[TAG_FRAME_CHIPS];
    float    ref[TAG_NSYM][NS];
    float    eref[TAG_NSYM];
    /* finding it */
    float    fold[FLEN];
    float    last[FLEN];                                     /* the latest metric at each start */
    int      best;
    float    sync_db, sync_now, sync_avg;
    uint64_t fresh_from;                                     /* after a reading, sums start again here */
    int      held;                                           /* the last reading's start, -1 none */
    bool     holding;                                        /* ...and it still stands out */
    /* levels */
    float    band_acc;
    int      lvl_syms;
    float    band_dbfs, all_dbfs, floor_dbfs;
    float    lvl[FLOOR_N];
    int      lvl_n, lvl_at;
    /* readings */
    tag_reading_t q[QUEUE];
    int      qn;
    struct sym {                                             /* a reading's symbols */
        int64_t fx;                                          /* where: from the try's start, 32.32 */
        float   zr, zi, p;
        float   dz;                                          /* this one against the one before, normalised */
        bool    ok, dok;
    } z[NFR][TAG_NSYM];
    memo_t   memo[NFR][TAG_NSYM];                            /* ...and their despreads, for all nine */
    uint32_t tries, readings, crc_bad;
};

size_t tag_dec_size(void) { return sizeof(tag_dec_t); }

/* A windowed-sinc low-pass at fs, DC gain 1. */
static void lowpass(float *h, int n, double fc, double fs)
{
    const double m = (n - 1) / 2.0, wc = 2 * fc / fs;
    double sum = 0;
    for (int j = 0; j < n; j++) {
        const double t = j - m;
        const double sinc = t == 0 ? wc : sin(M_PI * wc * t) / (M_PI * t);
        const double win = 0.42 - 0.5 * cos(2 * M_PI * j / (n - 1)) + 0.08 * cos(4 * M_PI * j / (n - 1));
        h[j] = (float)(sinc * win);
        sum += sinc * win;
    }
    for (int j = 0; j < n; j++) h[j] = (float)(h[j] / sum);
}

/* The reference: the frame's chips, shaped as the repeater shapes them, through
 * the decoder's own low-pass, at its 4 kHz. Made once, in the ring's memory
 * (not yet in use) for the 16 kHz pass between. */
static void make_ref(tag_dec_t *d)
{
    make_chips(d->chips);
    float g[SHAPE_TAPS];
    lowpass(g, SHAPE_TAPS, TAG_SHAPE_HZ, TAG_FS);
    float *v = d->br;                                        /* FLEN16 floats: 2 * RING is more */
    for (int n = 0; n < FLEN16; n++) {
        float acc = 0;
        for (int j = 0; j < SHAPE_TAPS; j++) {
            const int at = ((n - j) % FLEN16 + FLEN16) % FLEN16;
            acc += g[j] * (float)d->chips[at / SPC16];
        }
        v[n] = acc;
    }
    /* Both filters' delays taken off, so symbol k's chips fall in its own NS. */
    const int delay = (SHAPE_TAPS - 1) / 2 + (NTAP - 1) / 2;
    for (int k = 0; k < TAG_NSYM; k++) {
        double e = 0;
        for (int i = 0; i < NS; i++) {
            const int n = (k * NS + i) * DEC + delay;
            float acc = 0;
            for (int j = 0; j < NTAP; j++) {
                const int at = ((n - j) % FLEN16 + FLEN16) % FLEN16;
                acc += d->h[j] * v[at];
            }
            d->ref[k][i] = acc;
            e += (double)acc * acc;
        }
        d->eref[k] = (float)e;
    }
    memset(d->br, 0, sizeof d->br);
}

void tag_dec_init(tag_dec_t *d)
{
    memset(d, 0, sizeof *d);
    conv_table();
    const double w = 2 * M_PI * TAG_FC_HZ / TAG_FS;
    d->w_re = (float)cos(w);
    d->w_im = (float)-sin(w);
    d->c_re = 1;
    lowpass(d->h, NTAP, LP_HZ, TAG_FS);
    make_ref(d);
    d->band_dbfs = d->all_dbfs = d->floor_dbfs = -120;
    d->held = -1;
}

static float db(double p) { return p > 1e-12 ? (float)(10 * log10(p)) : -120.0f; }

/* The ring, at absolute 4 kHz sample a (still in it). */
static inline const float *ring_re(const tag_dec_t *d, uint64_t a) { return &d->br[a & RMASK]; }
static inline const float *ring_im(const tag_dec_t *d, uint64_t a) { return &d->bi[a & RMASK]; }
static inline float ring_pw(const tag_dec_t *d, uint64_t end) { return d->wp[end & RMASK]; }

/* Symbol k's reference against the NS samples from a. Four sums each way, so
 * the knob's FPU has four multiply-adds in flight instead of waiting on one,
 * and the loads at fixed offsets from pointers stepped once a round: written
 * plainly, it compiled to 14 instructions for every two multiply-adds, and
 * the sync search, 2048 of them for every one of 4000 starts a second, was
 * most of the decoder's time. */
static inline void despread(const tag_dec_t *d, int k, uint64_t a, float *zr, float *zi)
{
    const float *re = ring_re(d, a), *im = ring_im(d, a), *r = d->ref[k];
    float r0 = 0, r1 = 0, r2 = 0, r3 = 0, i0 = 0, i1 = 0, i2 = 0, i3 = 0;
    for (int n = NS / 4; n; n--, r += 4, re += 4, im += 4) {
        r0 += r[0] * re[0];
        i0 += r[0] * im[0];
        r1 += r[1] * re[1];
        i1 += r[1] * im[1];
        r2 += r[2] * re[2];
        i2 += r[2] * im[2];
        r3 += r[3] * re[3];
        i3 += r[3] * im[3];
    }
    *zr = (r0 + r1) + (r2 + r3);
    *zi = (i0 + i1) + (i2 + i3);
}

/* The 16 known symbols from lag q: how well they match, 0..15. */
static float sync_metric(const tag_dec_t *d, uint64_t q)
{
    float pr = 0, pi = 0, sum_r = 0, sum_i = 0;
    for (int k = 0; k < TAG_NSYNC; k++) {
        float zr, zi;
        despread(d, k, q + (uint64_t)(k * NS), &zr, &zi);
        const float p = ring_pw(d, q + (uint64_t)(k * NS + NS - 1)) * d->eref[k];
        const float s = p > 1e-20f ? 1.0f / sqrtf(p) : 0.0f;
        zr *= s;
        zi *= s;
        if (k) {
            /* z_k * conj(z_k-1), turned round where the sync bit says the sign flips */
            const float cr = zr * pr + zi * pi, ci = zi * pr - zr * pi;
            if (sync_bit(k)) { sum_r -= cr; sum_i -= ci; }
            else             { sum_r += cr; sum_i += ci; }
        }
        pr = zr;
        pi = zi;
    }
    return sqrtf(sum_r * sum_r + sum_i * sum_i);
}

static void level_done(tag_dec_t *d)
{
    d->band_dbfs = db(4.0 * d->band_acc / (LVL_SYMS * NS));
    d->all_dbfs = db(2.0 * d->all_acc / (LVL_SYMS * NS * DEC));
    d->band_acc = 0;
    d->all_acc = 0;
    d->lvl[d->lvl_at] = d->band_dbfs;
    d->lvl_at = (d->lvl_at + 1) % FLOOR_N;
    if (d->lvl_n < FLOOR_N) d->lvl_n++;
    float f = 0;
    for (int i = 0; i < d->lvl_n; i++)
        if (i == 0 || d->lvl[i] < f) f = d->lvl[i];
    d->floor_dbfs = f;
}

/* --- reading at a start ------------------------------------------------------------ */

typedef struct sym sym_t;

/* A try reads at nine places: three clocks (the repeater's sample rate the
 * same, or TAG_DRIFT_PPM off either way) and three starts (the best, a sample
 * either side), in this order. */
static const double DRIFTS[] = { 0, DRIFT, -DRIFT };
static const float  NUDGES[] = { 0, 1, -1 };
#define NDRIFT ((int)(sizeof DRIFTS / sizeof DRIFTS[0]))
#define NNUDGE ((int)(sizeof NUDGES / sizeof NUDGES[0]))
#define NHYP   (NDRIFT * NNUDGE)

/* Positions are counted from the try's start, in 32.32 fixed point: in double
 * -- software on the knob -- working out a few thousand of them a try cost as
 * much as all the despreading. */
#define FX_ONE 4294967296.0

typedef struct {
    int64_t at[NFR];                                         /* frame -f's symbol TAG_NSYNC-1 */
    int64_t step;                                            /* ...and from one symbol to the next */
} hyp_t;

static int64_t fx_of(double x) { return (int64_t)llround(x * FX_ONE); }

/* Symbol k of frame -f where the nine tries from start s put it (fx), each
 * sample despread once and all in one go: they are a few samples apart, and
 * nine passes over four frames of the ring -- 336 kB of PSRAM each on the knob
 * -- were most of a try's time. */
static void fill_memo(tag_dec_t *d, uint64_t s, const int64_t fx[NHYP], int f, int k)
{
    memo_t *m = &d->memo[f][k];
    int64_t lo = fx[0], hi = fx[0];
    for (int h = 1; h < NHYP; h++) {
        if (fx[h] < lo) lo = fx[h];
        if (fx[h] > hi) hi = fx[h];
    }
    m->have = 0;
    const int64_t top = (int64_t)s + (hi >> 32) + 1;         /* the last one's next sample */
    int64_t a0 = (int64_t)s + (lo >> 32);
    if (top < 0) return;
    if (a0 < 0) a0 = 0;
    m->lo = (uint64_t)a0;
    const uint64_t nb = d->nb;
    for (uint64_t a = m->lo; a <= (uint64_t)top && a - m->lo < MEMO; a++) {
        if (a + NS > nb || a + RING < nb) continue;          /* not all in, or gone */
        const unsigned i = (unsigned)(a - m->lo);
        despread(d, k, a, &m->zr[i], &m->zi[i]);
        m->have |= (uint16_t)(1u << i);
    }
}

/* Symbol k at sample a: from the memo, or (never yet) worked out. */
static inline void spread_at(const tag_dec_t *d, int k, const memo_t *m, uint64_t a, float *zr, float *zi)
{
    const uint64_t i = a - m->lo;                            /* huge when a < lo */
    if (i < MEMO && (m->have >> i & 1u)) {
        *zr = m->zr[i];
        *zi = m->zi[i];
        return;
    }
    despread(d, k, a, zr, zi);
}

/* Symbol k at fx from start s, between the two samples around it. */
static void read_sym(const tag_dec_t *d, int k, uint64_t s, int64_t fx, const memo_t *m, sym_t *z)
{
    const uint64_t nb = d->nb;
    z->ok = false;
    const int64_t ia = (int64_t)s + (fx >> 32);
    if (ia < 0) return;
    const uint64_t a = (uint64_t)ia;
    if (a + NS + 1 > nb || a + RING < nb + 2) return;        /* not all in, or gone */
    const float t = (float)(uint32_t)fx * (float)(1.0 / FX_ONE);
    float r0, i0, r1, i1;
    spread_at(d, k, m, a, &r0, &i0);
    spread_at(d, k, m, a + 1, &r1, &i1);
    z->zr = (1 - t) * r0 + t * r1;
    z->zi = (1 - t) * i0 + t * i1;
    z->p = (1 - t) * ring_pw(d, a + NS - 1) + t * ring_pw(d, a + NS);
    z->fx = fx;
    z->ok = true;
}

static bool decode_soft(const float D[TAG_NSYM], tag_reading_t *out)
{
    float soft[TAG_NBITS];
    for (int i = 0; i < TAG_NBITS; i++) soft[i] = -D[TAG_NSYNC + tag_perm(i)];
    uint8_t bits[TAG_PAYLOAD_BITS];
    tag_viterbi(soft, bits);
    if (!tag_unpack(bits, out->call, &out->local)) return false;
    uint8_t c[TAG_NBITS];
    tag_conv(bits, c);
    int wrong = 0;
    for (int i = 0; i < TAG_NBITS; i++)
        if (soft[i] == 0 || (soft[i] > 0) != (c[i] != 0)) wrong++;
    out->corrected = (uint8_t)wrong;
    return true;
}

/* Every symbol of the last 1..3 frames' time at one of a try's nine places:
 * the shortest that reads wins (it is the newest). */
static bool read_at(tag_dec_t *d, uint64_t s, const hyp_t *hy, double drift, tag_reading_t *out)
{
    sym_t (*z)[TAG_NSYM] = d->z;
    for (int f = 0; f < NFR; f++) {
        int64_t fx = hy->at[f];
        for (int k = TAG_NSYNC - 1; k < TAG_NSYM; k++, fx += hy->step) {
            sym_t *a = &z[f][k];
            read_sym(d, k, s, fx, &d->memo[f][k], a);
            a->dok = false;
            if (k < TAG_NSYNC || !a->ok) continue;
            const sym_t *b = &z[f][k - 1];
            if (!b->ok) continue;
            const float w = a->p * b->p;
            if (w <= 1e-30f) continue;
            a->dz = (a->zr * b->zr + a->zi * b->zi) / sqrtf(w);
            a->dok = true;
        }
    }
    const int64_t fd = (int64_t)d->fresh_from - (int64_t)s;
    const int64_t fresh = fd < -(INT64_C(1) << 30) ? INT64_MIN / 2 : fd * (INT64_C(1) << 32);
    for (int span = 1; span <= MAXSPAN; span++) {
        float D[TAG_NSYM] = { 0 };
        int got = 0;
        const int64_t from = fx_of((double)(d->nb - s) - span * FLEN * (1.0 + drift)) - ((int64_t)NS << 32);
        for (int f = 0; f < NFR; f++)
            for (int k = TAG_NSYNC; k < TAG_NSYM; k++) {
                const sym_t *a = &z[f][k];
                if (!a->dok || a->fx <= from || z[f][k - 1].fx < fresh) continue;
                D[k] += a->dz;
                got++;
            }
        if (got < TAG_NBITS / 2) continue;                   /* most of a frame, at least */
        if (decode_soft(D, out)) {
            out->frames = (uint8_t)span;
            return true;
        }
        if (span * FLEN > (int)d->nb) break;
    }
    return false;
}

/* The best start that is not within 3 of one already taken. */
static int next_best(const tag_dec_t *d, const int *taken, int n)
{
    int best = -1;
    for (int i = 0; i < FLEN; i++) {
        if (best >= 0 && d->fold[i] <= d->fold[best]) continue;
        bool near = false;
        for (int t = 0; t < n && !near; t++) {
            int dist = abs(i - taken[t]);
            if (dist > FLEN / 2) dist = FLEN - dist;
            near = dist < 3;
        }
        if (!near) best = i;
    }
    return best;
}

/* At start b: true and the reading in rd if one is there. */
static bool try_start(tag_dec_t *d, int b, tag_reading_t *rd)
{
    const float l = d->fold[(b + FLEN - 1) % FLEN], c = d->fold[b], r = d->fold[(b + 1) % FLEN];
    const float den = l - 2 * c + r;
    float frac = den < 0 ? 0.5f * (l - r) / den : 0.0f;
    if (frac > 0.5f) frac = 0.5f;
    if (frac < -0.5f) frac = -0.5f;
    /* The newest start whose 16 known symbols are all in. */
    const uint64_t last = d->nb - SEARCH - 1;
    const uint64_t s = last - ((last + FLEN - (uint64_t)b % FLEN) % FLEN);
    hyp_t hy[NHYP];
    for (int i = 0; i < NDRIFT; i++)
        for (int j = 0; j < NNUDGE; j++) {
            hyp_t *h = &hy[i * NNUDGE + j];
            const double g = 1.0 + DRIFTS[i];
            h->step = fx_of(NS * g);
            for (int f = 0; f < NFR; f++)
                h->at[f] = fx_of((double)frac + NUDGES[j] + ((double)(TAG_NSYNC - 1) * NS - (double)f * FLEN) * g);
        }
    for (int f = 0; f < NFR; f++) {
        int64_t fx[NHYP];
        for (int h = 0; h < NHYP; h++) fx[h] = hy[h].at[f];
        for (int k = TAG_NSYNC - 1; k < TAG_NSYM; k++) {
            fill_memo(d, s, fx, f, k);
            for (int h = 0; h < NHYP; h++) fx[h] += hy[h].step;
        }
    }
    for (int i = 0; i < NDRIFT; i++)
        for (int j = 0; j < NNUDGE; j++) {
            memset(rd, 0, sizeof *rd);
            if (read_at(d, s, &hy[i * NNUDGE + j], DRIFTS[i], rd)) {
                rd->drift = (int8_t)(i == 0 ? 0 : i == 1 ? 1 : -1);
                rd->nudge = (int8_t)NUDGES[j];
                return true;
            }
        }
    return false;
}

static void try_read(tag_dec_t *d)
{
    int taken[CANDS];
    tag_reading_t rd;
    d->tries++;
    for (int c = 0; c < CANDS; c++) {
        const int b = c ? next_best(d, taken, c) : d->best;
        if (b < 0) break;
        taken[c] = b;
        if (try_start(d, b, &rd)) {
            rd.at = d->nin;
            /* TailChaser: a sum belongs to one message, so after a reading the
             * next one is made of what is heard from here on -- a new talker
             * (at a frame's edge) is never added to the last one. */
            d->fresh_from = d->nb;
            d->held = b;
            d->holding = true;
            rd.sync_db = d->sync_db;
            rd.sync_avg = d->sync_avg;
            rd.sure = d->sync_avg >= TAG_SURE_AVG && rd.frames == 1 && tag_call_likely(rd.call);
            d->readings++;
            if (d->qn < QUEUE) d->q[d->qn++] = rd;
            else { memmove(d->q, d->q + 1, sizeof d->q[0] * (QUEUE - 1)); d->q[QUEUE - 1] = rd; }
            return;
        }
    }
    d->crc_bad++;
}

/* A symbol's worth of new baseband: the lags it completes, the level, and now
 * and then a reading. */
static void symbol_done(tag_dec_t *d)
{
    if (d->nb >= SEARCH + NS) {
        const uint64_t q0 = d->nb - SEARCH - NS;
        int at = (int)(q0 % FLEN);
        for (uint64_t q = q0; q < q0 + NS; q++) {
            const float m = sync_metric(d, q);
            d->fold[at] = FOLD_KEEP * d->fold[at] + m;
            d->last[at] = m;
            if (++at == FLEN) at = 0;
        }
    }
    if (++d->lvl_syms >= LVL_SYMS) {
        level_done(d);
        d->lvl_syms = 0;
    }
    if ((d->nb / NS) % TRY_EVERY || d->nb < FLEN) return;
    int best = 0;
    double sum = 0;
    for (int i = 0; i < FLEN; i += NS) {                     /* a float sum a symbol, the double once */
        float part = 0;
        for (int j = i; j < i + NS; j++) {
            part += d->fold[j];
            if (d->fold[j] > d->fold[best]) best = j;
        }
        sum += part;
    }
    d->best = best;
    const double mean = sum / FLEN;
    d->sync_db = mean > 0 ? (float)(10 * log10(d->fold[best] / mean)) : 0;
    d->sync_now = d->last[best] / (TAG_NSYNC - 1);
    d->sync_avg = d->fold[best] * (1 - FOLD_KEEP) / (TAG_NSYNC - 1);
    /* The last reading's tag is still on the air while its start stands out
     * (followed as the clocks drift apart). */
    if (d->held >= 0) {
        int dist = abs(best - d->held);
        if (dist > FLEN / 2) dist = FLEN - dist;
        d->holding = d->sync_avg >= TAG_TRY_AVG && dist <= 4;
        if (d->holding) d->held = best;
    }
    /* A frame's time of audio since the last reading, at least: every symbol
     * once. */
    if (d->sync_db >= TAG_TRY_DB && d->sync_avg >= TAG_TRY_AVG && d->sync_now >= TAG_TRY_NOW &&
        d->nb >= d->fresh_from + FLEN)
        try_read(d);
}

static void baseband(tag_dec_t *d, float re, float im)
{
    const uint64_t n = d->nb;
    const int at = (int)(n & RMASK);
    d->br[at] = d->br[at + RING] = re;
    d->bi[at] = d->bi[at + RING] = im;
    const float p = re * re + im * im;
    const int slot = (int)(n % NS);
    d->pw_sum += p - d->pw[slot];
    d->pw[slot] = p;
    if (slot == NS - 1) {                                    /* now and then, exactly */
        float s = 0;
        for (int i = 0; i < NS; i++) s += d->pw[i];
        d->pw_sum = s;
    }
    d->wp[at] = d->pw_sum;
    d->band_acc += p;
    d->nb = n + 1;
    if (d->nb % NS == 0) symbol_done(d);
}

static void sample(tag_dec_t *d, float x)
{
    d->all_acc += x * x;
    /* A trace of noise (-100 dBFS): digital silence, as a file or a muted
     * input gives it, is then a quiet microphone. Exact zeros made the starts
     * around them stand out against nothing. */
    d->dither = d->dither * 1664525u + 1013904223u;
    x += (float)((int32_t)d->dither >> 8) * (1e-5f / 8388608.0f);
    /* mix down: x * e^-jwn, the step's rounding taken out every NS*DEC samples */
    const float mr = x * d->c_re, mi = x * d->c_im;
    const float cr = d->c_re * d->w_re - d->c_im * d->w_im;
    d->c_im = d->c_re * d->w_im + d->c_im * d->w_re;
    d->c_re = cr;
    d->nin++;
    if (d->nin % (NS * DEC) == 0) {
        const double ph = fmod((double)d->nin * TAG_FC_HZ / TAG_FS, 1.0) * 2 * M_PI;
        d->c_re = (float)cos(ph);
        d->c_im = (float)-sin(ph);
    }
    d->xr[d->xp] = d->xr[d->xp + NTAP] = mr;
    d->xi[d->xp] = d->xi[d->xp + NTAP] = mi;
    d->xp = (d->xp + 1) % NTAP;
    if (d->nin % DEC) return;
    /* the newest is at xp-1: h[j] against x[newest - j] */
    const float *xr = &d->xr[d->xp], *xi = &d->xi[d->xp];   /* oldest first */
    float ar = 0, ai = 0;
    for (int j = 0; j < NTAP; j++) {
        ar += d->h[NTAP - 1 - j] * xr[j];
        ai += d->h[NTAP - 1 - j] * xi[j];
    }
    baseband(d, ar, ai);
}

int tag_dec_feed_f(tag_dec_t *d, const float *x, size_t n)
{
    for (size_t i = 0; i < n; i++) sample(d, x[i]);
    return d->qn;
}

int tag_dec_feed(tag_dec_t *d, const int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) sample(d, pcm[i] * (1.0f / 32768.0f));
    return d->qn;
}

bool tag_dec_take(tag_dec_t *d, tag_reading_t *out)
{
    if (!d->qn) return false;
    *out = d->q[0];
    memmove(d->q, d->q + 1, sizeof d->q[0] * (size_t)(QUEUE - 1));
    d->qn--;
    return true;
}

void tag_dec_stats(const tag_dec_t *d, tag_stats_t *st)
{
    st->band_dbfs = d->band_dbfs;
    st->all_dbfs = d->all_dbfs;
    st->floor_dbfs = d->floor_dbfs;
    st->sync_db = d->sync_db;
    st->sync_now = d->sync_now;
    st->sync_avg = d->sync_avg;
    st->holding = d->holding;
    st->tries = d->tries;
    st->readings = d->readings;
    st->crc_bad = d->crc_bad;
    st->heard = d->nin;
}
