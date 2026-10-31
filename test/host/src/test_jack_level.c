/* The jack's level (components/audio/jack_level.c): its steps and their
 * gains; above 0 dB the audio held back a block, exactly, and sample for
 * sample what the gain makes of it while that stays within full scale;
 * a loud passage at +12 dB turned down ahead of it, never past full scale,
 * never a jump, and back up after it; both ears by the same gain; and any
 * block size the playback task hands it. */
#include "tiny.h"
#include "jack_level.h"

#include <math.h>
#include <stdbool.h>

#define PI   3.14159265358979
#define RATE 24000

static void test_steps(void)
{
    CASE("the steps: 0 to +12 dB, in threes");
    int n = 0;
    for (long db = -20; db <= 20; db++)
        if (jack_level_ok(db)) {
            n++;
            CHECK_EQ(db % 3, 0);
        }
    CHECK_EQ(n, 5);
    CHECK(!jack_level_ok(-3) && !jack_level_ok(1) && !jack_level_ok(15));

    CASE("their gains: 0 dB is one, +6 twice, +12 four times; between, the nearest");
    CHECK_EQ(jack_level_gain(0), JACK_GAIN_ONE);
    CHECK_EQ(jack_level_gain(6), 2 * JACK_GAIN_ONE);
    CHECK_EQ(jack_level_gain(12), 4 * JACK_GAIN_ONE);
    for (int db = 0; db <= 12; db += 3) CHECK(fabs(20.0 * log10((double)jack_level_gain(db) / JACK_GAIN_ONE) - db) < 0.1);
    CHECK_EQ(jack_level_gain(1), jack_level_gain(0));
    CHECK_EQ(jack_level_gain(2), jack_level_gain(3));
    CHECK_EQ(jack_level_gain(-9), jack_level_gain(0));
    CHECK_EQ(jack_level_gain(40), jack_level_gain(12));
}

/* A second of stereo: the radio's tone left, another right, at amplitudes
 * `al` and `ar`, keyed on and off every `key_ms` (0: steady). */
static void tone(int16_t *s, int frames, double al, double ar, int key_ms)
{
    for (int i = 0; i < frames; i++) {
        const bool on = !key_ms || (i / (RATE * key_ms / 1000)) % 2 == 0;
        s[2 * i]     = on ? (int16_t)lround(al * sin(2 * PI * 700.0 * i / RATE)) : 0;
        s[2 * i + 1] = on ? (int16_t)lround(ar * sin(2 * PI * 1100.0 * i / RATE + 1.0)) : 0;
    }
}

/* `frames` through the limiter in blocks of `block`, into `out`. */
static bool run(int16_t *out, const int16_t *in, int frames, int block, int db)
{
    static jack_lim_t l;
    jack_lim_reset(&l);
    bool down = false;
    memcpy(out, in, (size_t)frames * 2 * sizeof *out);
    for (int at = 0; at < frames; at += block) {
        const int k = frames - at < block ? frames - at : block;
        down |= jack_lim_block(&l, out + 2 * at, (size_t)k, jack_level_gain(db));
    }
    return down;
}

static int16_t in_[RATE * 2], out_[RATE * 2];

static void test_quiet(void)
{
    CASE("quiet at +12 dB: held back JACK_AHEAD frames, then sample for sample four times it");
    tone(in_, RATE, 6000, 3000, 0);
    CHECK(!run(out_, in_, RATE, JACK_AHEAD, 12));
    int differ = 0;
    for (int i = 0; i < JACK_AHEAD; i++) differ += out_[2 * i] || out_[2 * i + 1];
    CHECK_EQ(differ, 0);                                    /* silence first: nothing held yet */
    for (int i = JACK_AHEAD; i < RATE; i++) {
        const int j = i - JACK_AHEAD;
        differ += out_[2 * i] != (int16_t)(4 * in_[2 * j]) || out_[2 * i + 1] != (int16_t)(4 * in_[2 * j + 1]);
    }
    CHECK_EQ(differ, 0);

    CASE("...the same whatever the block size the playback task hands it");
    static const int SIZES[] = { 1, 97, 240, 241, 300, 512 };
    for (size_t s = 0; s < sizeof SIZES / sizeof *SIZES; s++) {
        static int16_t o2[RATE * 2];
        CHECK(!run(o2, in_, RATE, SIZES[s], 12));
        CHECK(!memcmp(o2, out_, sizeof o2));
    }

    CASE("+6 dB: twice it");
    run(out_, in_, RATE, JACK_AHEAD, 6);
    differ = 0;
    for (int i = JACK_AHEAD; i < RATE; i++) differ += out_[2 * i] != (int16_t)(2 * in_[2 * (i - JACK_AHEAD)]);
    CHECK_EQ(differ, 0);
}

/* The gain each sample got: out over what came in JACK_AHEAD before, where
 * that is loud enough to say. */
static double gain_at(int i)
{
    const int j = i - JACK_AHEAD;
    if (j < 0 || abs(in_[2 * j]) < 3000) return -1;
    return (double)out_[2 * i] / in_[2 * j];
}

static void test_loud(int key_ms, int block, const char *what)
{
    CASE(what);
    tone(in_, RATE, 20000, 12000, key_ms);
    CHECK(run(out_, in_, RATE, block, 12));
    int wrong_sign = 0, past = 0;
    double hi = 0, jump = 0, prev = -1;
    for (int i = JACK_AHEAD; i < RATE; i++) {
        const int j = i - JACK_AHEAD;
        for (int c = 0; c < 2; c++) {
            /* never wrapped: the sign of what came in, and no more than four times it */
            if ((long)out_[2 * i + c] * in_[2 * j + c] < 0) wrong_sign++;
            if (abs(out_[2 * i + c]) > 4 * abs(in_[2 * j + c]) + 1) past++;
        }
        const double g = gain_at(i);
        if (g < 0) { prev = -1; continue; }
        if (g > hi) hi = g;
        if (prev >= 0 && fabs(g - prev) > jump) jump = fabs(g - prev);
        prev = g;
    }
    CHECK_EQ(wrong_sign, 0);
    CHECK_EQ(past, 0);
    CHECK(hi <= 4.0 + 1e-3);
    CHECK(jump < 0.02);                                     /* ramped, a sample at a time: no click */

    /* Its peak where it was turned down: full scale, not far under it. */
    int peak = 0;
    for (int i = RATE / 2; i < RATE; i++) peak = abs(out_[2 * i]) > peak ? abs(out_[2 * i]) : peak;
    CHECK(peak > 30000 && peak <= 32768);
}

static void test_balance(void)
{
    CASE("both ears by the same gain: the right as far down as the left, the balance kept");
    tone(in_, RATE, 20000, 4000, 0);
    run(out_, in_, RATE, JACK_AHEAD, 12);
    double worst = 0;
    for (int i = RATE / 2; i < RATE; i++) {
        const int j = i - JACK_AHEAD;
        if (abs(in_[2 * j]) < 5000 || abs(in_[2 * j + 1]) < 2000) continue;
        const double gl = (double)out_[2 * i] / in_[2 * j], gr = (double)out_[2 * i + 1] / in_[2 * j + 1];
        if (fabs(gl - gr) > worst) worst = fabs(gl - gr);
    }
    CHECK(worst < 0.01);
}

static void test_back_up(void)
{
    CASE("back up after a loud passage, a 32nd a block: four times again within a second");
    static jack_lim_t l;
    jack_lim_reset(&l);
    tone(in_, RATE, 20000, 20000, 0);
    for (int at = 0; at < RATE; at += JACK_AHEAD) jack_lim_block(&l, in_ + 2 * at, JACK_AHEAD, jack_level_gain(12));
    CHECK(l.r < JACK_GAIN_ONE / 2);
    tone(in_, RATE, 2000, 2000, 0);
    bool down = true;
    int blocks = 0;
    for (int at = 0; at < RATE && down; at += JACK_AHEAD, blocks++) {
        down = jack_lim_block(&l, in_ + 2 * at, JACK_AHEAD, jack_level_gain(12));
    }
    CHECK(!down);
    CHECK_EQ(l.r, JACK_GAIN_ONE);
    CHECK(blocks > 10 && blocks < 100);                     /* smoothly: not at once, not for ever */
}

int main(void)
{
    test_steps();
    test_quiet();
    test_loud(0, JACK_AHEAD, "a steady loud tone at +12 dB: down to full scale, never past it, never a jump");
    test_loud(60, JACK_AHEAD, "keyed CW at +12 dB, every 60 ms: never past full scale, never a jump");
    test_loud(60, 97, "...in blocks of 97 frames");
    test_balance();
    test_back_up();
    printf("%d checks, %d failed\n", t_run, t_fail);
    return t_fail ? 1 : 0;
}
