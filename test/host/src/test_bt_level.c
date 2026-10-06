/* A Bluetooth headset's or speaker's level (components/bt_link/bt_level.c):
 * its steps and their gains; the defaults -- a speaker the quarter it was
 * always sent, the JLab untouched, a headset the jack's level; the gain the
 * device is sent with, the level on top of the VOLUME or of the full-level
 * path, and its swell; at 0 dB and below sample for sample, and above it a
 * block held back and the gain turned down ahead of a loud passage, never
 * past full scale and never a jump -- keyed CW and speech at +12 dB, where
 * a block brought down whole by itself, as first built, clicked; and the
 * eight devices the levels are kept for, newest first. */
#include "tiny.h"
#include "bt_level.h"
#include "bt_link_proto.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>

#define N  240                          /* bt_link's block: 10 ms at 24 kHz */
#define PI 3.14159265358979

static double db_of(int32_t g) { return 20.0 * log10((double)g / BT_GAIN_ONE); }

/* The tap's sample before levels: the two channels' mean at a gain in
 * thousandths of full scale (bt_link.c, v1.18.5). */
static int16_t old_sample(int16_t l, int16_t r, int32_t g)
{
    return (int16_t)((((int32_t)l + r) / 2) * g / 1000);
}

/* ...and now, at a gain of BT_GAIN_ONE for full scale. */
static int16_t new_sample(int16_t l, int16_t r, int32_t g)
{
    return (int16_t)((int64_t)(((int32_t)l + r) / 2) * g / BT_GAIN_ONE);
}

static void test_steps(void)
{
    CASE("the steps: -24 to +12 dB, in threes");
    int n = 0;
    for (long db = -40; db <= 40; db++)
        if (bt_level_ok(db)) {
            n++;
            CHECK_EQ((db - BT_LEVEL_MIN) % 3, 0);
        }
    CHECK_EQ(n, 13);
    CHECK(bt_level_ok(-24) && bt_level_ok(-12) && bt_level_ok(0) && bt_level_ok(3) && bt_level_ok(12));
    static const long BAD[] = { -27, -25, -23, -13, -1, 1, 2, 13, 15, 120, LONG_MIN, LONG_MAX };
    for (size_t i = 0; i < sizeof BAD / sizeof *BAD; i++) CHECK(!bt_level_ok(BAD[i]));

    CASE("their gains: 0 dB is one, -12 a quarter exactly, +12 four times");
    CHECK_EQ(bt_level_gain(0), BT_GAIN_ONE);
    CHECK_EQ(bt_level_gain(-12), BT_GAIN_ONE / 4);
    CHECK_EQ(bt_level_gain(-6), BT_GAIN_ONE / 2);
    CHECK_EQ(bt_level_gain(-24), BT_GAIN_ONE / 16);
    CHECK_EQ(bt_level_gain(6), BT_GAIN_ONE * 2);
    CHECK_EQ(bt_level_gain(12), BT_GAIN_ONE * 4);

    CASE("each step 3 dB, every level within a tenth of a dB of what it says");
    for (int db = BT_LEVEL_MIN; db <= BT_LEVEL_MAX; db += BT_LEVEL_STEP) {
        CHECK(fabs(db_of(bt_level_gain(db)) - db) < 0.1);
        if (db < BT_LEVEL_MAX) {
            const double step = db_of(bt_level_gain(db + 3)) - db_of(bt_level_gain(db));
            CHECK(step > 3.0 && step < 3.02);
        }
    }

    CASE("between the steps the nearest, past the ends the end");
    CHECK_EQ(bt_level_gain(-23), bt_level_gain(-24));
    CHECK_EQ(bt_level_gain(-22), bt_level_gain(-21));
    CHECK_EQ(bt_level_gain(1), bt_level_gain(0));
    CHECK_EQ(bt_level_gain(2), bt_level_gain(3));
    CHECK_EQ(bt_level_gain(-100), bt_level_gain(-24));
    CHECK_EQ(bt_level_gain(100), bt_level_gain(12));
}

static void test_defaults(void)
{
    CASE("a speaker's level until one is set: 12 dB down; a headset's: 0 dB");
    CHECK_EQ(bt_level_default(BTL_KIND_SPEAKER), -12);
    CHECK_EQ(bt_level_default(BTL_KIND_HEADSET), 0);
    CHECK_EQ(bt_level_default(7), 0);              /* a kind not known here: a headset, as everywhere */
    CHECK(bt_level_ok(BT_LEVEL_SPEAKER) && bt_level_ok(BT_LEVEL_HEADSET));
}

static void test_want(void)
{
    static const int16_t M[] = { -32768, -20001, -12345, -3, -1, 0, 1, 2, 777, 16383, 32767 };

    CASE("a headset at its level: the jack's VOLUME, as it always was");
    int worst = 0;
    for (int v = 0; v <= 100; v++) {
        const int32_t g = bt_level_want((uint8_t)v, false, 0xFF, BT_LEVEL_HEADSET);
        CHECK_EQ(g, v * BT_GAIN_ONE / 100);
        for (size_t i = 0; i < sizeof M / sizeof *M; i++) {
            const int d = abs(new_sample(M[i], M[i], g) - old_sample(M[i], M[i], v * 10));
            if (d > worst) worst = d;
        }
    }
    CHECK(worst <= 1);                              /* a least bit at the most */

    CASE("a speaker at its level: a quarter of that, as it always was");
    double off = 0;
    for (int v = 0; v <= 100; v++) {
        const int32_t g = bt_level_want((uint8_t)v, false, 0xFF, BT_LEVEL_SPEAKER);
        CHECK_EQ(g, bt_level_want((uint8_t)v, false, 0xFF, 0) / 4);
        /* The old one in thousandths, a quarter of them cut short: within one. */
        const double d = fabs((double)g / BT_GAIN_ONE - (double)(v * 10 / 4) / 1000.0);
        if (d > off) off = d;
    }
    CHECK(off < 0.001);

    CASE("the full-level path at a speaker's level: a quarter of full scale, the JLab's as on 2026-10-05");
    CHECK_EQ(bt_level_want(2, true, btl_av_from_knob(2), BT_LEVEL_SPEAKER), BT_GAIN_ONE / 4);
    for (int v = 1; v <= 100; v++) CHECK_EQ(bt_level_want((uint8_t)v, true, 0xFF, BT_LEVEL_SPEAKER), BT_GAIN_ONE / 4);
    /* Sample for sample what the tap sent it, every sample there is. */
    int differ = 0;
    for (int32_t m = -32768; m <= 32767; m++)
        differ += new_sample((int16_t)m, (int16_t)m, BT_GAIN_ONE / 4) != old_sample((int16_t)m, (int16_t)m, 1000 / 4);
    CHECK_EQ(differ, 0);

    CASE("...less in proportion what it says it plays above the VOLUME asked");
    const uint8_t asked = btl_av_from_knob(40);
    CHECK_EQ(asked, 51);
    CHECK_EQ(bt_level_want(40, true, 102, 0), BT_GAIN_ONE / 2);                /* twice as loud: half */
    CHECK_EQ(bt_level_want(40, true, 102, BT_LEVEL_SPEAKER), BT_GAIN_ONE / 8);
    CHECK_EQ(bt_level_want(40, true, asked, 0), BT_GAIN_ONE);                  /* where asked */
    CHECK_EQ(bt_level_want(40, true, 20, 0), BT_GAIN_ONE);                     /* quieter: full */
    CHECK_EQ(bt_level_want(40, true, 200, 0), BT_GAIN_ONE);                    /* not a volume: unsaid */

    CASE("a VOLUME of 0: silence, whatever the level and the path");
    CHECK_EQ(bt_level_want(0, false, 0xFF, BT_LEVEL_MAX), 0);
    CHECK_EQ(bt_level_want(0, true, 0, BT_LEVEL_MAX), 0);
    CHECK_EQ(bt_level_want(0, true, 0xFF, BT_LEVEL_MAX), 0);

    CASE("the level on top: the Sony at 0 dB four times the quarter, at +12 sixteen times");
    CHECK_EQ(bt_level_want(30, true, 0xFF, 0), BT_GAIN_ONE);
    CHECK_EQ(bt_level_want(30, true, 0xFF, 12), 4 * BT_GAIN_ONE);
    CHECK_EQ(bt_level_want(30, true, 0xFF, -24), BT_GAIN_ONE / 16);
    CHECK_EQ(bt_level_want(50, false, 0xFF, 6), BT_GAIN_ONE);                  /* half the VOLUME, 6 dB up */
    CHECK_EQ(bt_level_want(100, false, 0xFF, 12), 4 * BT_GAIN_ONE);

    CASE("a VOLUME past 100: 100");
    CHECK_EQ(bt_level_want(200, false, 0xFF, 0), BT_GAIN_ONE);
    CHECK_EQ(bt_level_want(255, true, 0xFF, 0), BT_GAIN_ONE);
}

/* A swell from silence into `want`, its gain block by block into g[]: how
 * many blocks it took. */
static int swell_into(int32_t want, int32_t *g, int max)
{
    int32_t v  = 0;
    bool    sw = true;
    int     n  = 0;
    while (sw && n < max) g[n++] = v = bt_level_swell(v, want, &sw);
    return n;
}

/* ...as the tap swelled before levels, in thousandths of full scale. */
static int old_swell_into(int32_t want, int32_t *g, int max)
{
    int32_t v  = 0;
    bool    sw = true;
    int     n  = 0;
    while (sw && n < max) {
        if (want <= v) {
            v  = want;
            sw = false;
        } else if ((v += v / 32 + 1) >= want) {
            v  = want;
            sw = false;
        }
        g[n++] = v;
    }
    return n;
}

static void test_swell(void)
{
    static int32_t now[1000], before[1000];
    bool sw = false;

    CASE("not swelling: there at once, up or down");
    CHECK_EQ(bt_level_swell(0, BT_GAIN_ONE / 4, &sw), BT_GAIN_ONE / 4);
    CHECK_EQ(bt_level_swell(BT_GAIN_ONE / 4, 100, &sw), 100);
    CHECK(!sw);

    CASE("into full level at a speaker's level: block for block the swell it always was");
    const int n = swell_into(BT_GAIN_ONE / 4, now, 1000), was = old_swell_into(250, before, 1000);
    printf("a swell into a quarter of full scale: %d blocks of 10 ms, %d before levels\n", n, was);
    CHECK_EQ(n, was);
    int apart = 0;
    for (int i = 0; i < n && i < was; i++) apart += now[i] != (before[i] * BT_GAIN_ONE + 999) / 1000;
    CHECK_EQ(apart, 0);
    CHECK_EQ(now[n - 1], BT_GAIN_ONE / 4);

    CASE("into full level at +12 dB: under two seconds, never faster than a 32nd and a thousandth a block");
    const int n12 = swell_into(4 * BT_GAIN_ONE, now, 1000);
    printf("a swell into four times full scale: %d blocks of 10 ms\n", n12);
    CHECK(n12 > 150 && n12 < 200);
    CHECK_EQ(now[n12 - 1], 4 * BT_GAIN_ONE);
    int fast = 0;
    for (int i = 1; i < n12; i++) fast += now[i] > now[i - 1] + now[i - 1] / 32 + BT_GAIN_ONE / 1000 + 3;
    CHECK_EQ(fast, 0);

    CASE("a swell begun from a gain already there: on from it");
    sw = true;
    const int32_t g = bt_level_swell(BT_GAIN_ONE / 10, BT_GAIN_ONE, &sw);
    CHECK(sw && g > BT_GAIN_ONE / 10 && g < BT_GAIN_ONE / 10 + BT_GAIN_ONE / 200);

    CASE("turned down while it swells: down at once, the swell over");
    sw = true;
    CHECK_EQ(bt_level_swell(1000, 500, &sw), 500);
    CHECK(!sw);
}

/* 1 kHz at 24 kHz in the left channel, 700 Hz in the right, at their
 * crests `l` and `r`. */
static void tone(int16_t *stereo, double l, double r)
{
    for (int i = 0; i < N; i++) {
        stereo[2 * i]     = (int16_t)lrint(l * sin(2.0 * PI * 1000.0 * i / 24000.0));
        stereo[2 * i + 1] = (int16_t)lrint(r * sin(2.0 * PI * 700.0 * i / 24000.0 + 0.3));
    }
}

static int32_t mean(const int16_t *stereo, int i) { return ((int32_t)stereo[2 * i] + stereo[2 * i + 1]) / 2; }

/* Samples clipped flat: one at full scale beside the last, also there. */
static int flat(const int16_t *mono, int n)
{
    int k = 0;
    for (int i = 1; i < n; i++)
        k += (mono[i] >= 32766 && mono[i - 1] >= 32766) || (mono[i] <= -32767 && mono[i - 1] <= -32767);
    return k;
}

static int16_t sat16(int64_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

/* `len` frames through the level, in the tap's blocks, into `out`: how many
 * blocks it turned down. */
static int run(bt_level_lim_t *l, const int16_t *stereo, int16_t *out, int len, int32_t g, bool ahead)
{
    int turned = 0;
    for (int b = 0; b < len; b += N) {
        const int n = len - b < N ? len - b : N;
        turned += bt_level_block(l, out + b, stereo + 2 * b, (size_t)n, g, ahead);
    }
    return turned;
}

/* The largest step the gain makes from one sample to the next, by what it
 * moves the sample: |g(i) - g(i-1)| |x(i)|, of full scale. The gain read
 * back from what was sent, `y`, for the means `x`, where both samples are
 * loud enough to read it from: to a 2000th, a step to some 16 bits in 32768
 * at the most -- a click is thousands. */
static double worst_step(const int16_t *x, const int16_t *y, int len)
{
    double worst = 0;
    for (int i = 1; i < len; i++) {
        if (abs(x[i]) < 2000 || abs(x[i - 1]) < 2000) continue;
        const double s = fabs((double)y[i] / x[i] - (double)y[i - 1] / x[i - 1]) * abs(x[i]) / 32768.0;
        if (s > worst) worst = s;
    }
    return worst;
}

/* Never past full scale, the sign never turned over: each sample sent no
 * further from zero than its mean at the gain, and on its side. */
static int wrong(const int16_t *x, const int16_t *y, int len, int32_t g)
{
    int k = 0;
    for (int i = 0; i < len; i++)
        k += (int64_t)x[i] * y[i] < 0 || llabs((int64_t)y[i]) > llabs((int64_t)x[i] * g / BT_GAIN_ONE) + 1;
    return k;
}

static void test_block(void)
{
    static int16_t st[2 * N], mono[N], clipped[N];
    static bt_level_lim_t l;

    CASE("at 0 dB and below: these frames, sample for sample, the two channels' mean at the gain");
    bt_level_reset(&l);
    tone(st, 32767.0, 32767.0);
    CHECK(!bt_level_block(&l, mono, st, N, BT_GAIN_ONE, false));
    int same = 0;
    for (int i = 0; i < N; i++) same += mono[i] == mean(st, i);
    CHECK_EQ(same, N);
    tone(st, 30000.0, 12000.0);
    CHECK(!bt_level_block(&l, mono, st, N, BT_GAIN_ONE / 4, false));
    same = 0;
    for (int i = 0; i < N; i++) same += mono[i] == new_sample(st[2 * i], st[2 * i + 1], BT_GAIN_ONE / 4);
    CHECK_EQ(same, N);

    CASE("the very top and bottom of full scale: as they are");
    st[0] = st[1] = 32767;
    st[2] = st[3] = -32768;
    CHECK(!bt_level_block(&l, mono, st, N, BT_GAIN_ONE, false));
    CHECK_EQ(mono[0], 32767);
    CHECK_EQ(mono[1], -32768);

    CASE("the mean as the tap always took it, toward zero");
    tone(st, 0.0, 0.0);
    st[0] = 1;  st[1] = 0;                          /* 0.5 */
    st[2] = -1; st[3] = 0;                          /* -0.5 */
    st[4] = -3; st[5] = 0;                          /* -1.5 */
    CHECK(!bt_level_block(&l, mono, st, N, BT_GAIN_ONE, false));
    CHECK_EQ(mono[0], 0);
    CHECK_EQ(mono[1], 0);
    CHECK_EQ(mono[2], -1);

    CASE("above 0 dB, a gain of full scale at most: never turned down, the block before sample for sample");
    static int16_t prev[N];
    bt_level_reset(&l);
    for (int b = 0; b < 4; b++) {
        for (int i = 0; i < N; i++) st[2 * i] = st[2 * i + 1] = (i + b) % 2 ? 32767 : -32768;
        CHECK(!bt_level_block(&l, mono, st, N, BT_GAIN_ONE, true));
        same = 0;
        for (int i = 0; i < N; i++) same += mono[i] == (b ? prev[i] : 0);
        CHECK_EQ(same, N);
        for (int i = 0; i < N; i++) prev[i] = (int16_t)mean(st, i);
    }

    CASE("+12 dB, quiet blocks: four times, sample for sample, a block late");
    bt_level_reset(&l);
    tone(st, 6000.0, 2000.0);
    CHECK(!bt_level_block(&l, mono, st, N, 4 * BT_GAIN_ONE, true));
    int zero = 0;
    for (int i = 0; i < N; i++) zero += mono[i] == 0;
    CHECK_EQ(zero, N);                              /* nothing held yet */
    for (int i = 0; i < N; i++) prev[i] = (int16_t)mean(st, i);
    tone(st, 0.0, 0.0);
    CHECK(!bt_level_block(&l, mono, st, N, 4 * BT_GAIN_ONE, true));
    same = 0;
    for (int i = 0; i < N; i++) same += mono[i] == 4 * prev[i];
    CHECK_EQ(same, N);

    CASE("+12 dB, a loud block: down whole, its peak at full scale, its shape kept, never clipped");
    bt_level_reset(&l);
    tone(st, 20000.0, 9000.0);
    int32_t peak = 0;
    for (int i = 0; i < N; i++) {
        const int32_t m = mean(st, i);
        if (abs(m) > peak) peak = abs(m);
        clipped[i] = sat16((int64_t)m * 4);
        prev[i]    = (int16_t)m;
    }
    /* Turned down in the block before it -- here, the silence held -- and
     * sent at that. */
    CHECK(bt_level_block(&l, mono, st, N, 4 * BT_GAIN_ONE, true));
    zero = 0;
    for (int i = 0; i < N; i++) zero += mono[i] == 0;
    CHECK_EQ(zero, N);
    tone(st, 0.0, 0.0);
    CHECK(bt_level_block(&l, mono, st, N, 4 * BT_GAIN_ONE, true));
    const double k = 32767.0 / peak;
    double worst = 0;
    int    top = 0;
    for (int i = 0; i < N; i++) {
        const double d = fabs(mono[i] - k * prev[i]);
        if (d > worst) worst = d;
        if (abs(mono[i]) > top) top = abs(mono[i]);
    }
    CHECK(worst < 1.5);                             /* every sample alike, to the gain's rounding: no clipping */
    CHECK(top >= 32760 && top <= 32768);            /* below zero, full scale is -32768 */
    const int flat_clipped = flat(clipped, N), flat_now = flat(mono, N);
    printf("+12 dB on a loud block: %d of %d samples flat when clipped, %d when turned down\n",
           flat_clipped, N, flat_now);
    CHECK(flat_clipped > N / 4);
    CHECK_EQ(flat_now, 0);

    CASE("past full scale below zero: the same");
    bt_level_reset(&l);
    tone(st, 3000.0, 3000.0);
    st[200] = st[201] = -30000;
    for (int i = 0; i < N; i++) prev[i] = (int16_t)mean(st, i);
    CHECK(bt_level_block(&l, mono, st, N, 4 * BT_GAIN_ONE, true));
    tone(st, 0.0, 0.0);
    CHECK(bt_level_block(&l, mono, st, N, 4 * BT_GAIN_ONE, true));
    CHECK(mono[100] >= -32768 && mono[100] <= -32760);
    CHECK(abs(mono[1] - (int)(prev[1] * 32767.0 / 30000.0)) <= 1);

    CASE("a hair past full scale: down to it, either way");
    bt_level_reset(&l);
    tone(st, 0.0, 0.0);
    st[0] = st[1] = 32767;
    st[2] = st[3] = -32768;
    bt_level_block(&l, mono, st, N, BT_GAIN_ONE + 4, true);
    tone(st, 0.0, 0.0);
    CHECK(bt_level_block(&l, mono, st, N, BT_GAIN_ONE + 4, true));
    CHECK_EQ(mono[0], 32767);
    CHECK(mono[1] == -32768 || mono[1] == -32767);

    CASE("silence, and a gain of 0 -- a VOLUME of 0: silence, never turned down");
    bt_level_reset(&l);
    tone(st, 0.0, 0.0);
    mono[0] = 123;
    CHECK(!bt_level_block(&l, mono, st, N, 4 * BT_GAIN_ONE, true));
    zero = 0;
    for (int i = 0; i < N; i++) zero += mono[i] == 0;
    CHECK_EQ(zero, N);
    tone(st, 32767.0, 32767.0);
    for (int b = 0; b < 3; b++) {
        CHECK(!bt_level_block(&l, mono, st, N, 0, true));
        zero = 0;
        for (int i = 0; i < N; i++) zero += mono[i] == 0;
        CHECK_EQ(zero, N);
    }

    CASE("the level crossing 0 dB: turned on, the block held sent again; turned off, not sent");
    static int16_t a[2 * N], b2[2 * N], c[2 * N], d[2 * N];
    bt_level_reset(&l);
    tone(a, 3000.0, 1000.0);
    tone(b2, 1000.0, 3000.0);
    tone(c, 2000.0, 2000.0);
    tone(d, 500.0, 4000.0);
    bt_level_block(&l, mono, a, N, BT_GAIN_ONE, false);
    bt_level_block(&l, mono, b2, N, BT_GAIN_ONE, false);
    same = 0;
    for (int i = 0; i < N; i++) same += mono[i] == mean(b2, i);
    CHECK_EQ(same, N);
    bt_level_block(&l, mono, c, N, BT_GAIN_ONE, true);
    same = 0;
    for (int i = 0; i < N; i++) same += mono[i] == mean(b2, i);
    CHECK_EQ(same, N);                              /* b again */
    bt_level_block(&l, mono, d, N, BT_GAIN_ONE, false);
    same = 0;
    for (int i = 0; i < N; i++) same += mono[i] == mean(d, i);
    CHECK_EQ(same, N);                              /* c never */

    CASE("more frames than a block at once: as in blocks");
    static int16_t big[2 * 512], one_go[512], in_blocks[512];
    for (int i = 0; i < 512; i++) {
        big[2 * i]     = (int16_t)lrint(24000.0 * sin(2.0 * PI * 900.0 * i / 24000.0));
        big[2 * i + 1] = (int16_t)lrint(9000.0 * sin(2.0 * PI * 300.0 * i / 24000.0));
    }
    bt_level_reset(&l);
    for (int r = 0; r < 3; r++) bt_level_block(&l, one_go, big, 512, 3 * BT_GAIN_ONE, true);
    static bt_level_lim_t l2;
    bt_level_reset(&l2);
    for (int r = 0; r < 3; r++) {
        bt_level_block(&l2, in_blocks, big, N, 3 * BT_GAIN_ONE, true);
        bt_level_block(&l2, in_blocks + N, big + 2 * N, N, 3 * BT_GAIN_ONE, true);
        bt_level_block(&l2, in_blocks + 2 * N, big + 4 * N, 512 - 2 * N, 3 * BT_GAIN_ONE, true);
    }
    CHECK(!memcmp(one_go, in_blocks, sizeof one_go));
    CHECK(l.r == l2.r && !memcmp(l.held, l2.held, sizeof l.held));
}

/* The gain each block of N would have had as first built: its own, down
 * whole by 32767 over its peak where it passed full scale -- into `y`. */
static void as_built(const int16_t *x, int16_t *y, int len, int32_t g)
{
    for (int b = 0; b < len; b += N) {
        int32_t pk = 0;
        for (int i = b; i < b + N; i++) pk = abs(x[i]) > pk ? abs(x[i]) : pk;
        const int32_t gb = pk && (int64_t)pk * g / BT_GAIN_ONE > 32767 ? (int32_t)((int64_t)32767 * BT_GAIN_ONE / pk) : g;
        for (int i = b; i < b + N; i++) y[i] = (int16_t)((int64_t)x[i] * gb / BT_GAIN_ONE);
    }
}

static double urand(uint64_t *s)
{
    *s = *s * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)(*s >> 11) / 9007199254740992.0;
}

static void test_loud(void)
{
    enum { LEN = 24000 * 4 };
    static int16_t st[2 * LEN], x[LEN], y[LEN + N], old[LEN];
    static bt_level_lim_t l;
    const int32_t g12 = bt_level_want(50, true, 0xFF, 12);

    /* Keyed CW at -6 dBFS, 733 Hz, some 22 WPM with 5 ms edges, its
     * elements anywhere in the blocks -- the verifier's (2026-10-06). */
    const int dit = 1300, rise = 120;
    for (int i = 0; i < LEN; i++) {
        const int q = i % (2 * dit);
        double    e = 0;
        if (q < dit) {
            e = 1;
            if (q < rise) e = 0.5 - 0.5 * cos(PI * q / rise);
            if (dit - q < rise) e = 0.5 - 0.5 * cos(PI * (dit - q) / rise);
        }
        st[2 * i] = st[2 * i + 1] = x[i] = (int16_t)lrint(16384.0 * e * sin(2 * PI * 733.0 * i / 24000.0));
    }
    CASE("CW at +12 dB: never past full scale, never a jump; as first built, half of full scale");
    for (int db = 3; db <= 12; db += 3) {
        const int32_t g = bt_level_want(50, true, 0xFF, db);
        bt_level_reset(&l);
        const int turned = run(&l, st, y, LEN, g, true);
        const int16_t *sent = y + N;                /* a block late */
        as_built(x, old, LEN, g);
        const double now = worst_step(x, sent, LEN - N), then = worst_step(x, old, LEN);
        int top = 0;
        for (int i = 0; i < LEN - N; i++) top = abs(sent[i]) > top ? abs(sent[i]) : top;
        printf("CW at %+3d dB: %3d of %d blocks turned down; the gain's largest step %.4f of full scale "
               "(%.3f as first built); its loudest %d\n", db, turned, LEN / N, now, then, top);
        CHECK(now < 0.002);
        CHECK_EQ(wrong(x, sent, LEN - N, g), 0);
        CHECK_EQ(flat(sent, LEN - N), 0);
        if (db >= 9) {
            CHECK(then > 0.25);                     /* what this is for */
            CHECK(top >= 32700);                    /* at full scale, no lower */
        }
        if (db == 3) CHECK_EQ(turned, 0);           /* -3 dBFS at the most: room */
    }

    /* Noise bursts under a syllabic envelope, peaks at -6 dBFS: speech,
     * near enough -- the verifier's too. */
    uint64_t seed = 7;
    double   env = 0, tgt = 0, mx = 0, lp = 0;
    static double v[LEN];
    int seg = 0;
    for (int i = 0; i < LEN; i++) {
        if (--seg <= 0) {
            seg = (int)(24000 * (0.08 + 0.25 * urand(&seed)));
            tgt = urand(&seed) < 0.25 ? 0.02 : 0.2 + 0.8 * urand(&seed);
        }
        env += (tgt - env) * (tgt > env ? 0.004 : 0.0015);
        lp += (urand(&seed) - 0.5 - lp) * 0.3;
        v[i] = env * (lp + 0.5 * sin(2 * PI * 400 * i / 24000.0));
        if (fabs(v[i]) > mx) mx = fabs(v[i]);
    }
    for (int i = 0; i < LEN; i++) st[2 * i] = st[2 * i + 1] = x[i] = (int16_t)lrint(v[i] / mx * 16384.0);
    CASE("speech at +12 dB: never past full scale, never a jump");
    bt_level_reset(&l);
    const int turned = run(&l, st, y, LEN, g12, true);
    as_built(x, old, LEN, g12);
    const double now = worst_step(x, y + N, LEN - N), then = worst_step(x, old, LEN);
    printf("speech at +12 dB: %d of %d blocks turned down; the gain's largest step %.4f of full scale "
           "(%.3f as first built)\n", turned, LEN / N, now, then);
    CHECK(now < 0.01);
    CHECK(then > 0.05);
    CHECK_EQ(wrong(x, y + N, LEN - N, g12), 0);

    CASE("after a loud passage, back up to all of the gain in under half a second, sample for sample there");
    /* A second of full scale, then a quiet tone. */
    for (int i = 0; i < LEN; i++) {
        const double a = i < 24000 ? 32000.0 : 4000.0;
        st[2 * i] = st[2 * i + 1] = x[i] = (int16_t)lrint(a * sin(2 * PI * 500.0 * i / 24000.0));
    }
    bt_level_reset(&l);
    run(&l, st, y, LEN, g12, true);
    int back = -1;
    for (int b = 24000 / N; b < LEN / N - 1 && back < 0; b++) {
        int all = 1;
        for (int i = b * N; i < (b + 1) * N; i++) all &= y[i + N] == (int16_t)((int64_t)x[i] * g12 / BT_GAIN_ONE);
        if (all) back = b - 24000 / N;
    }
    printf("back to all of +12 dB %d blocks after a loud second\n", back);
    CHECK(back > 0 && back <= 50);
    CHECK_EQ(wrong(x, y + N, LEN - N, g12), 0);
}

/* Made-up addresses. */
static const uint8_t SONY[6]  = { 0x02, 0x00, 0x00, 0x00, 0x5A, 0x01 };
static const uint8_t JLAB[6]  = { 0x02, 0x00, 0x00, 0x00, 0x1B, 0x02 };
static const uint8_t JABRA[6] = { 0x02, 0x00, 0x00, 0x00, 0xAB, 0x03 };

static void dev(uint8_t *bda, int i)
{
    static const uint8_t base[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x00 };
    memcpy(bda, base, 6);
    bda[5] = (uint8_t)i;
}

static void test_devices(void)
{
    bt_levels_t t, u;
    int db = 99;

    CASE("none set: each at its kind's default -- the JLab at a quarter, the headset at the jack's");
    bt_levels_load(&t, NULL, 0);
    CHECK_EQ(t.n, 0);
    CHECK(!bt_levels_get(&t, SONY, &db));
    CHECK_EQ(bt_levels_level(&t, SONY, BTL_KIND_SPEAKER), -12);
    CHECK_EQ(bt_levels_level(&t, JLAB, BTL_KIND_SPEAKER), -12);
    CHECK_EQ(bt_levels_level(&t, JABRA, BTL_KIND_HEADSET), 0);

    CASE("the Sony raised: its own from then on, whatever it is used as; the others untouched");
    CHECK(bt_levels_set(&t, SONY, 0));
    CHECK(bt_levels_get(&t, SONY, &db) && db == 0);
    CHECK_EQ(bt_levels_level(&t, SONY, BTL_KIND_SPEAKER), 0);
    CHECK_EQ(bt_levels_level(&t, SONY, BTL_KIND_HEADSET), 0);
    CHECK_EQ(bt_levels_level(&t, JLAB, BTL_KIND_SPEAKER), -12);
    CHECK_EQ(bt_levels_level(&t, JABRA, BTL_KIND_HEADSET), 0);

    CASE("the same again: nothing changed, nothing to save");
    CHECK(!bt_levels_set(&t, SONY, 0));
    CHECK_EQ(t.n, 1);

    CASE("not a step: refused, nothing changed");
    CHECK(!bt_levels_set(&t, SONY, 1));
    CHECK(!bt_levels_set(&t, SONY, 15));
    CHECK(!bt_levels_set(&t, SONY, -27));
    CHECK_EQ(bt_levels_level(&t, SONY, BTL_KIND_SPEAKER), 0);

    CASE("newest first; set again, to the front");
    CHECK(bt_levels_set(&t, JABRA, -3));
    CHECK(!memcmp(t.rec[0].bda, JABRA, 6) && !memcmp(t.rec[1].bda, SONY, 6));
    CHECK(bt_levels_set(&t, SONY, 3));
    CHECK(!memcmp(t.rec[0].bda, SONY, 6) && !memcmp(t.rec[1].bda, JABRA, 6));
    CHECK_EQ(t.rec[0].db, 3);
    CHECK_EQ(t.n, 2);

    CASE("forgotten: its kind's default again, the rest in order");
    CHECK(bt_levels_set(&t, JLAB, -6));
    CHECK(bt_levels_forget(&t, SONY));
    CHECK(!bt_levels_forget(&t, SONY));
    CHECK_EQ(t.n, 2);
    CHECK(!memcmp(t.rec[0].bda, JLAB, 6) && !memcmp(t.rec[1].bda, JABRA, 6));
    CHECK_EQ(bt_levels_level(&t, SONY, BTL_KIND_SPEAKER), -12);

    CASE("a ninth device: the oldest set goes");
    uint8_t b[6];
    bt_levels_load(&t, NULL, 0);
    for (int i = 0; i < BT_LEVEL_DEVICES; i++) {
        dev(b, i);
        CHECK(bt_levels_set(&t, b, -24 + 3 * i));
    }
    CHECK_EQ(t.n, BT_LEVEL_DEVICES);
    dev(b, 8);
    CHECK(bt_levels_set(&t, b, 12));
    CHECK_EQ(t.n, BT_LEVEL_DEVICES);
    dev(b, 0);
    CHECK(!bt_levels_get(&t, b, NULL));             /* the first set, gone */
    for (int i = 1; i <= 8; i++) {
        dev(b, i);
        CHECK(bt_levels_get(&t, b, NULL));
    }
    CASE("...one set again stays: the next oldest goes instead");
    dev(b, 1);
    CHECK(bt_levels_set(&t, b, 0));
    dev(b, 9);
    CHECK(bt_levels_set(&t, b, 0));
    dev(b, 1);
    CHECK(bt_levels_get(&t, b, &db) && db == 0);
    dev(b, 2);
    CHECK(!bt_levels_get(&t, b, NULL));

    CASE("to NVS and back: the same records, newest first");
    bt_levels_load(&u, t.rec, (size_t)t.n * sizeof t.rec[0]);
    CHECK_EQ(u.n, t.n);
    CHECK(!memcmp(u.rec, t.rec, (size_t)t.n * sizeof t.rec[0]));

    CASE("from NVS: what this firmware cannot take left out, a device once, eight at most");
    bt_level_rec_t blob[12];
    memset(blob, 0, sizeof blob);
    for (int i = 0; i < 12; i++) {
        dev(blob[i].bda, 20 + i);
        blob[i].db = -9;
    }
    blob[1].db    = 5;                              /* not a step */
    blob[2].db    = 15;                             /* past the top */
    memcpy(blob[3].bda, blob[0].bda, 6);            /* the first again */
    blob[3].db    = 3;
    blob[4].spare = 0x80;                           /* a later firmware's */
    bt_levels_load(&u, blob, sizeof blob);
    CHECK_EQ(u.n, BT_LEVEL_DEVICES);
    CHECK(u.rec[0].db == -9 && !memcmp(u.rec[0].bda, blob[0].bda, 6));
    CHECK(!memcmp(u.rec[1].bda, blob[4].bda, 6));
    CHECK(!bt_levels_get(&u, blob[1].bda, NULL) && !bt_levels_get(&u, blob[2].bda, NULL));
    CHECK(!memcmp(u.rec[7].bda, blob[10].bda, 6)); /* the eighth taken: 0, 4..10 */
    CASE("...a later firmware's byte kept through a set");
    CHECK(bt_levels_set(&u, blob[4].bda, -21));
    CHECK_EQ(u.rec[0].spare, 0x80);
    CHECK_EQ(u.rec[0].db, -21);

    CASE("from NVS: nothing, or less than a record");
    bt_levels_load(&u, blob, 7);
    CHECK_EQ(u.n, 0);
    bt_levels_load(&u, blob, sizeof blob[0] + 5);   /* a record and a piece */
    CHECK_EQ(u.n, 1);
}

T_MAIN(test_steps(); test_defaults(); test_want(); test_swell(); test_block(); test_loud(); test_devices())
