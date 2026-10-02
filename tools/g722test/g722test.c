/* The phone firmware's G.722 (components/phone_client/g722.c) on the host,
 * against ffmpeg's adpcm_g722 run as a black box -- its binary, never its
 * source:
 *   make test               (or ./g722test [-k] [-i appendix-ii-dir] [workdir])
 * Writes test signals as 16 kHz s16le files, has ffmpeg encode each one and
 * decode its own stream, then checks
 *   (a) our decoder on ffmpeg's stream gives ffmpeg's decode, sample for
 *       sample (fed in odd-sized pieces, as RTP packets come);
 *   (b) our encoder gives ffmpeg's octets (its default encoder, no trellis;
 *       ours fed 20 ms at a time, as the phone does);
 *   (c) our own round trip's SNR, beside ffmpeg's;
 * and prints a PASS/FAIL table. Exit status 0 when every row passes.
 *
 * Where (b) differs it is ffmpeg leaving out the Recommendation's transmit
 * QMF limit (clause 5.2.1, LOWT and HIGHT: XL and XH held to -16384..16383)
 * -- shown, not assumed: the same rows are encoded again with g722.c's own
 * blocks and that one limit left out, which must give ffmpeg's octets
 * exactly ("PASS*"). g722.c is included here, not linked, for those blocks.
 *
 * -i DIR also runs the Recommendation's own conformance test of its ADPCM,
 * Appendix II's digital test sequences (T1C1.XMT ... T3H3.RC0, mode 1; from
 * the ITU, not kept here), on g722.c's sub-band coders with the QMFs
 * by-passed, as the appendix has it.
 *
 * Scratch files go to a fresh directory under $TMPDIR (or /tmp), removed
 * afterwards; -k keeps them, a named workdir is kept too. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../../components/phone_client/g722.c"

#define RATE  16000
#define FRAME 320          /* 20 ms: what the phone encodes at a time */
#define DELAY 22           /* the QMFs', in samples: 11.5 each, less the one from XIN(j), a
                              * pair's second sample, to XOUT1, its first */
#define PI    3.14159265358979323846
#define FF    "ffmpeg -hide_banner -loglevel error -nostdin -y"

static const char *s_dir;
static int s_keep;

/* ------------------------------------------------------------- files */

static void path(char *p, size_t n, const char *name, const char *ext)
{
    snprintf(p, n, "%s/%s%s", s_dir, name, ext);
}

static void write_file(const char *p, const void *d, size_t n)
{
    FILE *f = fopen(p, "wb");
    if (!f || fwrite(d, 1, n, f) != n) { perror(p); exit(2); }
    fclose(f);
}

static void *read_file(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *d = malloc(sz > 0 ? (size_t)sz : 1);
    if (!d || (sz > 0 && fread(d, 1, (size_t)sz, f) != (size_t)sz)) { perror(p); exit(2); }
    fclose(f);
    *n = (size_t)sz;
    return d;
}

static void drop(const char *name, const char *ext)
{
    char p[512];
    path(p, sizeof p, name, ext);
    if (!s_keep) unlink(p);
}

static int run(const char *cmd)
{
    const int rc = system(cmd);
    if (rc != 0) fprintf(stderr, "failed (%d): %s\n", rc, cmd);
    return rc;
}

/* ------------------------------------------------------ test signals */

static uint64_t s_rng = 0x2545F4914F6CDD1Dull;

static uint64_t next64(void)            /* splitmix64 */
{
    uint64_t z = (s_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static double uni(void)                 /* [0, 1) */
{
    return (double)(next64() >> 11) * (1.0 / 9007199254740992.0);
}

static double gauss(void)
{
    const double u = uni() + 1e-300, v = uni();
    return sqrt(-2.0 * log(u)) * cos(2.0 * PI * v);
}

static double db(double dbfs) { return 32768.0 * pow(10.0, dbfs / 20.0); }

static double *buf(int n) { return calloc((size_t)n, sizeof(double)); }

/* Doubles to int16, rounded and clipped; scaled to `rms` first if > 0. */
static int16_t *finish(double *v, int n, double rms)
{
    if (rms > 0) {
        double e = 0;
        for (int i = 0; i < n; i++) e += v[i] * v[i];
        const double g = e > 0 ? rms / sqrt(e / n) : 0;
        for (int i = 0; i < n; i++) v[i] *= g;
    }
    int16_t *x = malloc((size_t)n * sizeof *x);
    for (int i = 0; i < n; i++) {
        const double r = floor(v[i] + 0.5);
        x[i] = (int16_t)(r > 32767.0 ? 32767.0 : r < -32768.0 ? -32768.0 : r);
    }
    free(v);
    return x;
}

static int16_t *gen_silence(int n, const double *p)
{
    (void)p;
    return finish(buf(n), n, 0);
}

/* p[0]: the level, dBFS. */
static int16_t *gen_sweep(int n, const double *p)
{
    double *v = buf(n);
    const double f1 = 50, f2 = 7900, T = (double)n / RATE, k = log(f2 / f1);
    for (int i = 0; i < n; i++)
        v[i] = db(p[0]) * sin(2 * PI * f1 * T / k * (exp((double)i / RATE / T * k) - 1));
    return finish(v, n, 0);
}

/* p[0]: the rms, dBFS. */
static int16_t *gen_white(int n, const double *p)
{
    double *v = buf(n);
    for (int i = 0; i < n; i++) v[i] = gauss();
    return finish(v, n, db(p[0]));
}

/* p[0]: the rms, dBFS. Paul Kellet's filter: -3 dB an octave, within
 * 0.05 dB above 9 Hz. */
static int16_t *gen_pink(int n, const double *p)
{
    double *v = buf(n), b[7] = { 0 };
    for (int i = 0; i < n; i++) {
        const double w = gauss();
        b[0] = 0.99886 * b[0] + w * 0.0555179;
        b[1] = 0.99332 * b[1] + w * 0.0750759;
        b[2] = 0.96900 * b[2] + w * 0.1538520;
        b[3] = 0.86650 * b[3] + w * 0.3104856;
        b[4] = 0.55000 * b[4] + w * 0.5329522;
        b[5] = -0.7616 * b[5] - w * 0.0168980;
        v[i] = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362;
        b[6] = w * 0.115926;
    }
    return finish(v, n, db(p[0]));
}

/* p[0], p[1]: the tones, Hz; p[2]: each one's level, dBFS. */
static int16_t *gen_twotone(int n, const double *p)
{
    double *v = buf(n);
    for (int i = 0; i < n; i++)
        v[i] = db(p[2]) * (sin(2 * PI * p[0] * i / RATE) + sin(2 * PI * p[1] * i / RATE + 1.0));
    return finish(v, n, 0);
}

/* A 1 kHz sine 12 dB over full scale, clipped to a square. */
static int16_t *gen_clip_sine(int n, const double *p)
{
    (void)p;
    double *v = buf(n);
    for (int i = 0; i < n; i++) v[i] = 4.0 * 32768.0 * sin(2 * PI * 1000.0 * i / RATE);
    return finish(v, n, 0);
}

/* The two extremes alternating every sample -- all of it at 8 kHz, the
 * higher band's edge -- in 100 ms bursts, full-scale DC between. */
static int16_t *gen_clip_nyquist(int n, const double *p)
{
    (void)p;
    int16_t *x = malloc((size_t)n * sizeof *x);
    for (int i = 0; i < n; i++) {
        const int hi = (i / 1600) & 1 ? (i & 1) == 0 : (i / 3200) & 1;
        x[i] = hi ? 32767 : -32768;
    }
    return x;
}

/* Full-scale steps, each held 1 to 400 samples. */
static int16_t *gen_clip_steps(int n, const double *p)
{
    (void)p;
    int16_t *x = malloc((size_t)n * sizeof *x);
    int16_t v = 32767;
    for (int i = 0; i < n; ) {
        const int hold = 1 + (int)(uni() * 400);
        for (int k = 0; k < hold && i < n; k++) x[i++] = v;
        v = v > 0 ? -32768 : 32767;
    }
    return x;
}

/* Something like speech: syllables of a glottal pulse train (intonation,
 * jitter, aspiration; -12 dB an octave tilt, +6 lip radiation) through five
 * cascaded formants gliding between vowels, peaking at -6 dBFS; fricatives
 * (noise above 3.5 kHz) at -40 to -30 dBFS rms and plosive bursts at -30 to
 * -20 dBFS; pauses of digital silence or a quiet room (-65 dBFS). */
static int16_t *gen_speech(int n, const double *p)
{
    (void)p;
    static const double V[5][3] = {
        { 730, 1090, 2440 }, { 270, 2290, 3010 }, { 300, 870, 2240 },   /* a i u */
        { 530, 1840, 2480 }, { 570, 840, 2410 },                        /* e o */
    };
    static const double B[5] = { 70, 90, 120, 180, 250 };
    double *vo = buf(n), *uv = buf(n);
    double F[5] = { 500, 1500, 2500, 3500, 4500 }, from[3] = { 0 };
    double y1[5] = { 0 }, y2[5] = { 0 }, g1 = 0, g2 = 0, prev = 0, phase = 0;
    /* fricatives' high pass: second-order Butterworth at 3.5 kHz */
    const double K = tan(PI * 3500 / RATE), nm = 1 / (1 + K * sqrt(2.0) + K * K);
    const double a1 = 2 * (K * K - 1) * nm, a2 = (1 - K * sqrt(2.0) + K * K) * nm;
    double hx1 = 0, hx2 = 0, hy1 = 0, hy2 = 0;
    int i = 0;
    while (i < n) {
        const int pause = (int)(RATE * (0.05 + 0.3 * uni()));
        const double room = uni() < 0.5 ? 0 : db(-65);
        for (int k = 0; k < pause && i < n; k++) uv[i++] = room * gauss();
        const double r = uni();
        if (r < 0.35) {
            const int len = (int)(RATE * (0.06 + 0.1 * uni()));
            const double amp = db(-40 + 10 * uni()) * 1.6;      /* ~rms after the high pass */
            for (int k = 0; k < len && i < n; k++) {
                const double w = gauss() * amp * sin(PI * k / len);
                const double hp = nm * (w - 2 * hx1 + hx2) - a1 * hy1 - a2 * hy2;
                hx2 = hx1; hx1 = w; hy2 = hy1; hy1 = hp;
                uv[i++] = hp;
            }
        } else if (r < 0.55) {
            const double amp = db(-30 + 10 * uni());
            for (int k = 0; k < RATE / 100 && i < n; k++) uv[i++] = gauss() * amp * exp(-k / 30.0);
        }
        const int len = (int)(RATE * (0.12 + 0.2 * uni()));
        const double *to = V[(int)(uni() * 5) % 5];
        for (int f = 0; f < 3; f++) from[f] = F[f];
        const double f0 = 90 + 120 * uni(), drift = (uni() - 0.5) * 60, amp = 0.3 + 0.7 * uni();
        for (int k = 0; k < len && i < n; k++) {
            const double a = k < RATE / 20 ? (double)k / (RATE / 20) : 1.0;     /* a 50 ms glide */
            for (int f = 0; f < 3; f++) F[f] = from[f] + (to[f] - from[f]) * a;
            const double env = amp * (k < 320 ? k / 320.0 : 1.0) * (len - k < 800 ? (len - k) / 800.0 : 1.0);
            phase += (f0 + drift * k / len + 2.0 * gauss()) / RATE;
            double e = 0.02 * gauss();
            if (phase >= 1.0) { phase -= 1.0; e += 1.0; }
            g1 = 0.97 * g1 + e;
            g2 = 0.97 * g2 + g1;
            double s = g2 - prev;
            prev = g2;
            for (int f = 0; f < 5; f++) {
                const double rr = exp(-PI * B[f] / RATE), c = 2 * rr * cos(2 * PI * F[f] / RATE);
                const double o = (1 - c + rr * rr) * s + c * y1[f] - rr * rr * y2[f];   /* unity at DC */
                y2[f] = y1[f];
                y1[f] = o;
                s = o;
            }
            vo[i++] = s * env;
        }
    }
    double pk = 0;
    for (int k = 0; k < n; k++) pk = fmax(pk, fabs(vo[k]));
    for (int k = 0; k < n; k++) uv[k] += vo[k] * db(-6) / pk;
    free(vo);
    return finish(uv, n, 0);
}

typedef struct {
    const char *name, *what;
    int16_t *(*gen)(int n, const double *p);
    double p[3];
    double seconds;
} test_t;

static const test_t TESTS[] = {
    { "silence",         "digital silence",                             gen_silence,      { 0 },     2 },
    { "sweep_-3dBFS",    "log sine sweep 50-7900 Hz, -3 dBFS",          gen_sweep,        { -3 },   10 },
    { "sweep_-40dBFS",   "log sine sweep 50-7900 Hz, -40 dBFS",         gen_sweep,        { -40 },  10 },
    { "white_-60dBFS",   "white noise, -60 dBFS rms",                   gen_white,        { -60 },   4 },
    { "white_-30dBFS",   "white noise, -30 dBFS rms",                   gen_white,        { -30 },   4 },
    { "white_-12dBFS",   "white noise, -12 dBFS rms",                   gen_white,        { -12 },   4 },
    { "pink_-50dBFS",    "pink noise, -50 dBFS rms",                    gen_pink,         { -50 },   4 },
    { "pink_-30dBFS",    "pink noise, -30 dBFS rms",                    gen_pink,         { -30 },   4 },
    { "pink_-10dBFS",    "pink noise, -10 dBFS rms",                    gen_pink,         { -10 },   4 },
    { "twotone_1k_6k",   "1020 + 6010 Hz, -9 dBFS each",                gen_twotone,      { 1020, 6010, -9 }, 4 },
    { "twotone_3k9_4k1", "3900 + 4100 Hz (the QMF's edge), -9 dBFS each", gen_twotone,    { 3900, 4100, -9 }, 4 },
    { "clip_sine",       "1 kHz sine 12 dB over full scale, clipped",   gen_clip_sine,    { 0 },     2 },
    { "clip_nyquist",    "+-full scale at 8 kHz, full-scale DC between", gen_clip_nyquist, { 0 },    2 },
    { "clip_steps",      "full-scale steps, each held 1-400 samples",   gen_clip_steps,   { 0 },     4 },
    { "speech",          "speech-like: vowels, fricatives, bursts, pauses", gen_speech,   { 0 },    12 },
};

/* --------------------------------------------------------------- coders */

static double s_enc_s, s_dec_s;          /* our coder's time, for the record */
static long   s_frames;
static long   s_suppressed;              /* 0000xx codes we sent: must stay 0 */

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* Ours, 20 ms at a time. */
static uint8_t *our_encode(const int16_t *x, int n)
{
    uint8_t *o = malloc((size_t)n / 2 + 1);
    g722_enc_t e;
    g722_enc_init(&e);
    const double t0 = now();
    int m = 0;
    for (int i = 0; i < n; i += FRAME)
        m += g722_encode(&e, x + i, n - i < FRAME ? n - i : FRAME, o + m);
    s_enc_s += now() - t0;
    s_frames += n / FRAME;
    for (int i = 0; i < m; i++) s_suppressed += (o[i] & 63) < 4;
    return o;
}

/* Ours, in pieces of 1 to 200 octets. */
static int16_t *our_decode(const uint8_t *c, int n)
{
    int16_t *y = malloc((size_t)n * 2 * sizeof *y + 2);
    g722_dec_t d;
    g722_dec_init(&d);
    const double t0 = now();
    int m = 0;
    for (int i = 0; i < n; ) {
        int k = 1 + (int)(next64() % 200);
        if (k > n - i) k = n - i;
        m += g722_decode(&d, c + i, k, y + m);
        i += k;
    }
    s_dec_s += now() - t0;
    return y;
}

/* g722_encode() with clause 5.2.1's LOWT/HIGHT limit left out: XL and XH
 * reach the ADPCM as they come -- what ffmpeg's octets show it does. The
 * rest is g722.c's own blocks. *hits counts the octets where the limit
 * would have held XL or XH. */
static uint8_t *encode_unlimited(const int16_t *x, int n, long *hits)
{
    uint8_t *o = malloc((size_t)n / 2 + 1);
    g722_enc_t e;
    g722_enc_init(&e);
    *hits = 0;
    for (int j = 0; j + 1 < n; j += 2) {
        memmove(e.xin + 2, e.xin, 22 * sizeof e.xin[0]);
        e.xin[1] = x[j];
        e.xin[0] = x[j + 1];
        int32_t xa = 0, xb = 0;
        for (int i = 0; i < 24; i += 2) {
            xa += e.xin[i] * H[i];
            xb += e.xin[i + 1] * H[i + 1];
        }
        const int xl = (xa + xb) >> 14, xh = (xa - xb) >> 14;
        *hits += xl != limit(xl) || xh != limit(xh);
        const int il = encode_lo(&e.lo, xl);
        const int ih = encode_hi(&e.hi, xh);
        o[j / 2] = (uint8_t)(ih << 6 | il);
    }
    return o;
}

/* ---------------------------- ITU-T G.722 Appendix II, when it is given */

/* One of Appendix II's digital test sequences: 16-bit words in hex, sixteen
 * to a line, each line closed by a checksum (the two's complement of the
 * low byte of its characters' sum); comments between slash-stars. Returns
 * the words read, -1 if the file is missing or a checksum wrong. */
static int itu_load(const char *dir, const char *name, uint16_t *w, int max)
{
    char p[1024], line[256];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); return -1; }
    int n = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        int len = 0;
        while (isxdigit((unsigned char)line[len])) len++;
        if (line[0] == '/' || len < 6) continue;
        unsigned sum = 0, ck, v;
        for (int i = 0; i < len - 2; i++) sum += (unsigned char)line[i];
        sscanf(line + len - 2, "%2x", &ck);
        bad += ((sum + ck) & 0xFF) != 0;
        for (int i = 0; i + 4 <= len - 2 && n < max; i += 4) {
            sscanf(line + i, "%4x", &v);
            w[n++] = (uint16_t)v;
        }
    }
    fclose(f);
    if (bad) fprintf(stderr, "%s: %d lines fail their checksum\n", p, bad);
    return bad ? -1 : n;
}

/* Configuration 1, the encoder with its QMF by-passed: INFA's X# (XL << 1,
 * the reset RSS in its LSB; XH = XL) in, INFB's I# ((IH << 6 | IL) << 8, or
 * 1 while reset) out, against the reference. */
static int itu_encoder(const char *dir, const char *in, const char *ref)
{
    static uint16_t x[20000], r[20000];
    const int n = itu_load(dir, in, x, 20000), m = itu_load(dir, ref, r, 20000);
    g722_sb_t lo, hi;
    band_reset(&lo, 32);
    band_reset(&hi, 8);
    long bad = 0, first = -1;
    for (int i = 0; i < n && i < m; i++) {
        uint16_t o = 1;
        if (x[i] & 1) {
            band_reset(&lo, 32);
            band_reset(&hi, 8);
        } else {
            const int xl = (int16_t)x[i] >> 1;
            const int il = encode_lo(&lo, xl), ih = encode_hi(&hi, xl);
            o = (uint16_t)((ih << 6 | il) << 8);
        }
        if (o != r[i]) { if (first < 0) first = i; bad++; }
    }
    const int pass = n > 0 && n == m && !bad;
    printf("  encoder  %-9s -> %-30s %6d words  %s", in, ref, n, pass ? "PASS" : "FAIL");
    if (bad) printf(" (%ld differ, 1st @%ld)", bad, first);
    printf("\n");
    return !pass;
}

/* Configuration 2, the decoder with its QMF by-passed, in mode 1: INFC's
 * I# in, INFD's RL# and RH# (the output << 1, or 1 while reset) out,
 * against the references. */
static int itu_decoder(const char *dir, const char *in, const char *refl, const char *refh)
{
    static uint16_t c[20000], rl[20000], rh[20000];
    const int n = itu_load(dir, in, c, 20000);
    const int ml = itu_load(dir, refl, rl, 20000), mh = itu_load(dir, refh, rh, 20000);
    g722_sb_t lo, hi;
    band_reset(&lo, 32);
    band_reset(&hi, 8);
    long bad = 0, first = -1;
    for (int i = 0; i < n && i < ml && i < mh; i++) {
        uint16_t ol = 1, oh = 1;
        if (c[i] & 1) {
            band_reset(&lo, 32);
            band_reset(&hi, 8);
        } else {
            ol = (uint16_t)(decode_lo(&lo, (c[i] >> 8) & 63) * 2);
            oh = (uint16_t)(decode_hi(&hi, c[i] >> 14) * 2);
        }
        if (ol != rl[i] || oh != rh[i]) { if (first < 0) first = i; bad++; }
    }
    char refs[64];
    snprintf(refs, sizeof refs, "%s, %s", refl, refh);
    const int pass = n > 0 && n == ml && n == mh && !bad;
    printf("  decoder  %-9s -> %-30s %6d words  %s", in, refs, n, pass ? "PASS" : "FAIL");
    if (bad) printf(" (%ld differ, 1st @%ld)", bad, first);
    printf("\n");
    return !pass;
}

/* --------------------------------------------------------------- checks */

/* y's SNR against x, DELAY samples late, in dB. */
static double snr(const int16_t *x, const int16_t *y, int n)
{
    double es = 0, en = 0;
    for (int i = DELAY; i < n; i++) {
        const double e = (double)y[i] - x[i - DELAY];
        es += (double)x[i - DELAY] * x[i - DELAY];
        en += e * e;
    }
    return en > 0 ? 10 * log10(es / en) : 999;
}

static double rms(const int16_t *y, int n)
{
    double e = 0;
    for (int i = 0; i < n; i++) e += (double)y[i] * y[i];
    return sqrt(e / n);
}

static long differ(const void *a, const void *b, size_t n, size_t size, long *first)
{
    long bad = 0;
    *first = -1;
    for (size_t i = 0; i < n; i++)
        if (memcmp((const char *)a + i * size, (const char *)b + i * size, size)) {
            if (*first < 0) *first = (long)i;
            bad++;
        }
    return bad;
}

int main(int argc, char **argv)
{
    char tmpl[512];
    const char *itu = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-k")) s_keep = 1;
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) itu = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: %s [-k] [-i appendix-ii-dir] [workdir]\n", argv[0]);
            return 2;
        } else { s_dir = argv[i]; s_keep = 1; }
    }
    if (s_dir) mkdir(s_dir, 0755);
    else {
        const char *t = getenv("TMPDIR");
        snprintf(tmpl, sizeof tmpl, "%s/g722test.XXXXXX", t && *t ? t : "/tmp");
        if (!(s_dir = mkdtemp(tmpl))) { perror("mkdtemp"); return 2; }
    }
    if (run("ffmpeg -hide_banner -encoders 2>/dev/null | grep -q adpcm_g722")) {
        fprintf(stderr, "this needs ffmpeg, with adpcm_g722\n");
        return 2;
    }

    printf("G.722 mode 1, components/phone_client/g722.c against ffmpeg's adpcm_g722 (a black box)\n"
           "(a) our decoder on ffmpeg's stream == ffmpeg's decode, sample for sample\n"
           "(b) our encoder == ffmpeg's encoder, octet for octet; limit: octets where LOWT/HIGHT\n"
           "    held XL or XH to 15 bits; PASS* -- ffmpeg skips that limit: our blocks without\n"
           "    it give its octets exactly\n"
           "(c) round-trip SNR in dB, ours / ffmpeg's, %d samples' delay (silence: idle noise,\n"
           "    rms LSB); PASS within 1 dB of ffmpeg's\n\n", DELAY);
    printf("%-16s %-46s %6s  %-26s %6s %-27s %-24s\n", "signal", "what", "octets",
           "(a) decode", "limit", "(b) encode", "(c) round trip");
    int fails = 0, deviations = 0;
    const int ntests = (int)(sizeof TESTS / sizeof TESTS[0]);
    for (int t = 0; t <= ntests; t++) {
        char raw[512], cod[512], dec[512], cmd[2048];
        const char *name = t < ntests ? TESTS[t].name : "random_octets";
        path(raw, sizeof raw, name, ".raw");
        path(cod, sizeof cod, name, ".g722");
        path(dec, sizeof dec, name, ".dec.raw");
        int n;
        int16_t *x = NULL;
        if (t < ntests) {
            const test_t *T = &TESTS[t];
            n = (int)(T->seconds * RATE) / FRAME * FRAME;
            x = T->gen(n, T->p);
            write_file(raw, x, (size_t)n * 2);
            snprintf(cmd, sizeof cmd, FF " -f s16le -ar 16000 -ac 1 -i '%s' -c:a g722 -f g722 '%s'", raw, cod);
            if (run(cmd)) return 2;
        } else {
            /* Every octet the decoder can meet, the four codes never sent
             * among them: its saturations and limits, as a corrupted stream
             * would drive them. */
            n = 8 * RATE;
            uint8_t *r = malloc((size_t)n / 2);
            for (int i = 0; i < n / 2; i++) r[i] = (uint8_t)(next64() >> 56);
            write_file(cod, r, (size_t)n / 2);
            free(r);
        }
        snprintf(cmd, sizeof cmd, FF " -f g722 -i '%s' -f s16le -ar 16000 -ac 1 '%s'", cod, dec);
        if (run(cmd)) return 2;

        size_t nc, nd;
        uint8_t *ffc = read_file(cod, &nc);
        int16_t *ffd = read_file(dec, &nd);
        nd /= 2;
        long first, bad;

        /* (a) */
        char ra[96];
        int16_t *ourd = our_decode(ffc, (int)nc);
        const size_t na = nd < 2 * nc ? nd : 2 * nc;
        bad = differ(ourd, ffd, na, 2, &first);
        const int pass_a = bad == 0 && nd == 2 * nc;
        if (pass_a) snprintf(ra, sizeof ra, "PASS %zu/%zu", na, nd);
        else if (bad) snprintf(ra, sizeof ra, "FAIL %ld differ, 1st @%ld", bad, first);
        else snprintf(ra, sizeof ra, "FAIL %zu samples, not %zu", nd, 2 * nc);

        /* (b) */
        char rb[96] = "-", rl[24] = "-";
        int pass_b = 1;
        uint8_t *ourc = NULL;
        if (x) {
            long hits, first2;
            ourc = our_encode(x, n);
            uint8_t *unl = encode_unlimited(x, n, &hits);
            const size_t nb = nc < (size_t)n / 2 ? nc : (size_t)n / 2;
            const int lenok = nc == (size_t)n / 2;
            bad = differ(ourc, ffc, nb, 1, &first);
            const long bad2 = differ(unl, ffc, nb, 1, &first2);
            snprintf(rl, sizeof rl, "%ld", hits);
            if (lenok && !bad) snprintf(rb, sizeof rb, "PASS %zu/%zu", nb, nc);
            else if (lenok && !bad2 && hits) {
                snprintf(rb, sizeof rb, "PASS* %ld differ, 1st @%ld", bad, first);
                deviations++;
            } else {
                pass_b = 0;
                if (!lenok) snprintf(rb, sizeof rb, "FAIL %zu octets, not %d", nc, n / 2);
                else snprintf(rb, sizeof rb, "FAIL %ld differ, 1st @%ld", bad, first);
            }
            free(unl);
        }

        /* (c) */
        char rc[96] = "-";
        int pass_c = 1;
        if (x) {
            int16_t *rt = our_decode(ourc, n / 2);
            int silent = 1;
            for (int i = 0; i < n && silent; i++) silent = x[i] == 0;
            if (silent) {
                const double a = rms(rt, n), b = rms(ffd, (int)nd);
                pass_c = a <= b + 1e-9;
                snprintf(rc, sizeof rc, "%s %.2f / %.2f LSB", pass_c ? "PASS" : "FAIL", a, b);
            } else {
                const double a = snr(x, rt, n), b = snr(x, ffd, (int)nd < n ? (int)nd : n);
                pass_c = a >= b - 1.0;
                snprintf(rc, sizeof rc, "%s %6.2f / %6.2f", pass_c ? "PASS" : "FAIL", a, b);
            }
            free(rt);
        } else {
            long sat16 = 0;
            for (size_t i = 0; i < nd; i++) sat16 += ffd[i] == 32767 || ffd[i] == -32768;
            snprintf(rc, sizeof rc, "(%ld samples at full scale)", sat16);
        }

        printf("%-16s %-46s %6zu  %-26s %6s %-27s %-24s\n", name,
               t < ntests ? TESTS[t].what : "random octets, decoded only", nc, ra, rl, rb, rc);
        fails += !pass_a + !pass_b + !pass_c;
        free(x); free(ffc); free(ffd); free(ourd); free(ourc);
        drop(name, ".raw");
        drop(name, ".g722");
        drop(name, ".dec.raw");
    }
    if (!s_keep) rmdir(s_dir);
    else printf("\nfiles kept in %s\n", s_dir);

    printf("\nnever sent: the four suppressed codes 0000xx -- %s (%ld)\n",
           s_suppressed ? "FAIL" : "PASS", s_suppressed);
    fails += s_suppressed != 0;
    printf("host: %.1f us to encode, %.1f us to decode 20 ms (%ld frames)\n",
           1e6 * s_enc_s / (double)s_frames, 1e6 * s_dec_s / (double)s_frames, s_frames);
    if (itu) {
        printf("\nITU-T G.722 Appendix II digital test sequences, mode 1, QMFs by-passed (%s):\n", itu);
        fails += itu_encoder(itu, "T1C1.XMT", "T2R1.COD");
        fails += itu_encoder(itu, "T1C2.XMT", "T2R2.COD");
        fails += itu_decoder(itu, "T2R1.COD", "T3L1.RC1", "T3H1.RC0");
        fails += itu_decoder(itu, "T2R2.COD", "T3L2.RC1", "T3H2.RC0");
        fails += itu_decoder(itu, "T1D3.COD", "T3L3.RC1", "T3H3.RC0");
    }
    if (deviations)
        printf("PASS*: %d input%s where ffmpeg leaves out G.722 clause 5.2.1's limit of XL, XH to\n"
               "-16384..16383 (LOWT, HIGHT); g722.c keeps it\n", deviations, deviations == 1 ? "" : "s");
    printf("%s: %d check%s failed\n", fails ? "FAIL" : "PASS", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
