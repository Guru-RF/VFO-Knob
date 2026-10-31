/* The jack's level. See jack_level.h. */
#include "jack_level.h"

#include <string.h>

/* JACK_GAIN_ONE times 2^(k/2), k = 0..4: as bt_level.c's from 0 dB up. */
static const int32_t GAIN[] = { 65536, 92682, 131072, 185364, 262144 };
_Static_assert(sizeof GAIN / sizeof GAIN[0] == JACK_LEVEL_MAX / JACK_LEVEL_STEP + 1, "a gain a step");

bool jack_level_ok(long db)
{
    return db >= 0 && db <= JACK_LEVEL_MAX && db % JACK_LEVEL_STEP == 0;
}

int32_t jack_level_gain(int db)
{
    if (db < 0) db = 0;
    if (db > JACK_LEVEL_MAX) db = JACK_LEVEL_MAX;
    return GAIN[(db + JACK_LEVEL_STEP / 2) / JACK_LEVEL_STEP];
}

void jack_lim_reset(jack_lim_t *l)
{
    memset(l->held, 0, sizeof l->held);
    l->r = JACK_GAIN_ONE;
}

/* The highest and the lowest of `n` samples, 0 the least either way. */
static void span(const int16_t *s, size_t n, int32_t *hi, int32_t *lo)
{
    int32_t h = 0, w = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] > h) h = s[i];
        if (s[i] < w) w = s[i];
    }
    *hi = h;
    *lo = w;
}

/* The share of gain `g` that samples between `lo` and `hi` take within
 * full scale, JACK_GAIN_ONE for all of it: else their peak lands on full
 * scale. Exact, as the sample is taken (toward zero) -- bt_level.c's. */
static int32_t share(int32_t hi, int32_t lo, int32_t g)
{
    const int64_t one2 = (int64_t)JACK_GAIN_ONE * JACK_GAIN_ONE;
    int64_t       r    = JACK_GAIN_ONE;
    if ((int64_t)hi * g >= 32768LL * JACK_GAIN_ONE) r = (32768 * one2 - 1) / ((int64_t)hi * g);
    if ((int64_t)-lo * g >= 32769LL * JACK_GAIN_ONE) {
        const int64_t q = (32769 * one2 - 1) / ((int64_t)-lo * g);
        if (q < r) r = q;
    }
    return (int32_t)r;
}

bool jack_lim_block(jack_lim_t *l, int16_t *stereo, size_t n, int32_t g)
{
    bool down = false;
    while (n) {
        const size_t k  = n > JACK_AHEAD ? JACK_AHEAD : n;
        int16_t     *in = l->held + 2 * JACK_AHEAD;        /* these, after those held */
        memcpy(in, stereo, 2 * k * sizeof stereo[0]);
        /* The frames sent -- the k held longest -- and the JACK_AHEAD
         * after them, as bt_level_block's: the block's gain starts within
         * its own room and ends within the next one's too, so the next
         * block starts where this one ends. Back up a 32nd a block. */
        int32_t hi, lo;
        span(l->held, 2 * k, &hi, &lo);
        const int32_t now = share(hi, lo, g);
        span(l->held + 2 * k, 2 * JACK_AHEAD, &hi, &lo);
        const int32_t next = share(hi, lo, g);
        const int32_t r0   = l->r < now ? l->r : now;
        int32_t       r1   = r0 + r0 / 32 + 1;
        if (r1 > now) r1 = now;
        if (r1 > next) r1 = next;
        const int32_t g0 = (int32_t)((int64_t)g * r0 / JACK_GAIN_ONE);
        const int32_t g1 = (int32_t)((int64_t)g * r1 / JACK_GAIN_ONE);
        for (size_t i = 0; i < k; i++) {
            const int32_t gi = g0 + (g1 - g0) * (int32_t)(i + 1) / (int32_t)k;
            stereo[2 * i]     = (int16_t)((int64_t)l->held[2 * i] * gi / JACK_GAIN_ONE);
            stereo[2 * i + 1] = (int16_t)((int64_t)l->held[2 * i + 1] * gi / JACK_GAIN_ONE);
        }
        l->r = r1;
        down |= r0 < JACK_GAIN_ONE || r1 < JACK_GAIN_ONE;
        memmove(l->held, l->held + 2 * k, 2 * JACK_AHEAD * sizeof l->held[0]);
        stereo += 2 * k;
        n      -= k;
    }
    return down;
}
