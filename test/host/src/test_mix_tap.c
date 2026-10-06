/* The tap's block (components/audio/mix_tap.h): the mix as a Bluetooth
 * headset or speaker takes it, before the volume. Within full scale it is
 * what it always was, sample for sample; past it -- a strong station starting
 * under the leveller's 8x, still held for a weak one -- the block comes down
 * whole, its shape kept, where it was clipped flat. */
#include "tiny.h"
#include "mix_tap.h"

#include <math.h>

#define N  480                      /* mix_block's 10 ms, stereo */
#define PI 3.14159265358979f

/* audio_out.c's sat16: the block as the tap had it before. */
static int16_t sat16(float v) { return v > 32767.0f ? 32767 : (v < -32768.0f ? -32768 : (int16_t)v); }

/* 1 kHz at 24 kHz in the left ear, 700 Hz in the right, `amp` and `amp_r`
 * at their crests. */
static void tone(float *mix, float amp, float amp_r)
{
    for (int i = 0; i < N / 2; i++) {
        mix[2 * i]     = amp * sinf(2.0f * PI * 1000.0f * (float)i / 24000.0f);
        mix[2 * i + 1] = amp_r * sinf(2.0f * PI * 700.0f * (float)i / 24000.0f + 0.3f);
    }
}

/* Samples clipped flat: one at full scale beside its channel's last, also
 * there. A crest may touch it; a run of them is clipping. */
static int flat(const int16_t *out)
{
    int n = 0;
    for (int i = 2; i < N; i++)
        n += (out[i] >= 32766 && out[i - 2] >= 32766) || (out[i] <= -32767 && out[i - 2] <= -32767);
    return n;
}

static void test_within(void)
{
    static float mix[N];
    static int16_t out[N];
    CASE("within full scale: as it always was");
    tone(mix, 30000.0f, 12000.0f);
    mix[10] = 32767.0f;                     /* its very top, and below zero its very bottom */
    mix[11] = -32768.0f;
    mix[12] = -0.7f;                        /* toward zero, as the cast rounds */
    mix[13] = 0.99f;
    mix_tap(out, mix, N);
    int same = 0;
    for (int i = 0; i < N; i++) same += out[i] == sat16(mix[i]);
    CHECK_EQ(same, N);
    CHECK_EQ(out[10], 32767);
    CHECK_EQ(out[11], -32768);
    CASE("a hair past full scale: down a hair, not clipped");
    mix[11] = -32769.0f;
    mix_tap(out, mix, N);
    CHECK(out[11] == -32767 || out[11] == -32766);
    CHECK(out[10] == 32766 || out[10] == 32765);
}

static void test_past(void)
{
    static float mix[N];
    static int16_t out[N], clipped[N];
    CASE("past full scale: brought down whole, its shape kept");
    /* A station at -4 dBFS starting while the leveller still holds 8x. */
    tone(mix, 8.0f * 20000.0f, 8.0f * 9000.0f);
    float peak = 0.0f;
    for (int i = 0; i < N; i++) peak = fabsf(mix[i]) > peak ? fabsf(mix[i]) : peak;
    for (int i = 0; i < N; i++) clipped[i] = sat16(mix[i]);
    mix_tap(out, mix, N);
    const float k = 32767.0f / peak;
    float worst = 0.0f;
    for (int i = 0; i < N; i++) {
        const float d = fabsf((float)out[i] - k * mix[i]);
        if (d > worst) worst = d;
    }
    CHECK(worst < 1.0f);                    /* every sample scaled alike: no clipping, no other change */
    int top = 0;
    for (int i = 0; i < N; i++) top = abs(out[i]) > top ? abs(out[i]) : top;
    CHECK(top >= 32760 && top <= 32767);    /* its peak at full scale, not over */
    const int flat_before = flat(clipped), flat_now = flat(out);
    printf("an 8x block: %d of %d samples clipped flat before, %d now\n", flat_before, N, flat_now);
    CHECK(flat_before > N / 4);             /* what the headset heard: a cracking square wave */
    CHECK_EQ(flat_now, 0);                  /* now its crests only touch full scale */
    /* The two ears keep their balance: the right still under half the left. */
    int top_r = 0;
    for (int i = 1; i < N; i += 2) top_r = abs(out[i]) > top_r ? abs(out[i]) : top_r;
    CHECK(abs(top_r - (int)(9000.0f / 20000.0f * 32767.0f)) < 200);
}

static void test_below_zero(void)
{
    static float mix[N];
    static int16_t out[N];
    CASE("the peak below zero");
    tone(mix, 20000.0f, 10000.0f);
    mix[100] = -90000.0f;                   /* one sample far out, below zero */
    mix_tap(out, mix, N);
    CHECK(out[100] >= -32767 && out[100] <= -32760);
    CHECK(abs(out[0] - (int)(mix[0] * 32767.0f / 90000.0f)) <= 1);
    int top = 0;
    for (int i = 0; i < N; i++) top = abs(out[i]) > top ? abs(out[i]) : top;
    CHECK(top <= 32767);
}

static void test_silence(void)
{
    static float mix[N];
    static int16_t out[N];
    CASE("silence: silence");
    for (int i = 0; i < N; i++) mix[i] = 0.0f;
    out[0] = 123;
    mix_tap(out, mix, N);
    int zero = 0;
    for (int i = 0; i < N; i++) zero += out[i] == 0;
    CHECK_EQ(zero, N);
}

T_MAIN(test_within(); test_past(); test_below_zero(); test_silence())
