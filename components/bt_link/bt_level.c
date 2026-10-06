/* A Bluetooth headset's or speaker's level. See bt_level.h. */
#include "bt_level.h"

#include <string.h>

#include "bt_link_proto.h"

/* BT_GAIN_ONE times 2^(k/2), k = -8..4: each step the square root of two,
 * 3.01 dB, the odd ones to the nearest. -12 dB is a quarter exactly. */
static const int32_t GAIN[] = {
    4096, 5793, 8192, 11585, 16384, 23170, 32768, 46341, 65536, 92682, 131072, 185364, 262144,
};
_Static_assert(sizeof GAIN / sizeof GAIN[0] == (BT_LEVEL_MAX - BT_LEVEL_MIN) / BT_LEVEL_STEP + 1, "a gain a step");

bool bt_level_ok(long db)
{
    return db >= BT_LEVEL_MIN && db <= BT_LEVEL_MAX && (db - BT_LEVEL_MIN) % BT_LEVEL_STEP == 0;
}

int bt_level_default(uint8_t kind)
{
    return kind == BTL_KIND_SPEAKER ? BT_LEVEL_SPEAKER : BT_LEVEL_HEADSET;
}

int32_t bt_level_gain(int db)
{
    if (db < BT_LEVEL_MIN) db = BT_LEVEL_MIN;
    if (db > BT_LEVEL_MAX) db = BT_LEVEL_MAX;
    return GAIN[(db - BT_LEVEL_MIN + BT_LEVEL_STEP / 2) / BT_LEVEL_STEP];
}

int32_t bt_level_want(uint8_t volume, bool full, uint8_t at, int db)
{
    if (volume > 100) volume = 100;
    int64_t want = (int64_t)volume * BT_GAIN_ONE / 100;
    if (full && volume) {
        const uint8_t asked = btl_av_from_knob(volume);
        want = at > asked && at <= 127 ? (int64_t)BT_GAIN_ONE * asked / at : BT_GAIN_ONE;
    }
    return (int32_t)(want * bt_level_gain(db) / BT_GAIN_ONE);
}

int32_t bt_level_swell(int32_t g, int32_t want, bool *swell)
{
    if (!*swell || want <= g) {
        *swell = false;
        return want;
    }
    /* In thousandths of full scale, as the tap always counted it: the same
     * steps, so a swell takes as long as it always did. Up to the next
     * whole one, so that the next block reads back the thousandth it got. */
    int32_t t = (int32_t)((int64_t)g * 1000 / BT_GAIN_ONE);
    t += t / 32 + 1;
    g = (int32_t)(((int64_t)t * BT_GAIN_ONE + 999) / 1000);
    if (g >= want) {
        *swell = false;
        return want;
    }
    return g;
}

void bt_level_reset(bt_level_lim_t *l)
{
    memset(l->held, 0, sizeof l->held);
    l->r = BT_GAIN_ONE;
}

/* The highest and the lowest of `n` means, 0 the least either way. */
static void span(const int16_t *m, size_t n, int32_t *hi, int32_t *lo)
{
    int32_t h = 0, w = 0;
    for (size_t i = 0; i < n; i++) {
        if (m[i] > h) h = m[i];
        if (m[i] < w) w = m[i];
    }
    *hi = h;
    *lo = w;
}

/* The share of gain `g` that means between `lo` and `hi` take within full
 * scale, BT_GAIN_ONE for all of it: else their peak lands on full scale.
 * Exact, as the sample is taken (toward zero): all of it exactly where the
 * gain alone passes no sample past 32767 or -32768. */
static int32_t share(int32_t hi, int32_t lo, int32_t g)
{
    const int64_t one2 = (int64_t)BT_GAIN_ONE * BT_GAIN_ONE;
    int64_t       r    = BT_GAIN_ONE;
    if ((int64_t)hi * g >= 32768LL * BT_GAIN_ONE) r = (32768 * one2 - 1) / ((int64_t)hi * g);
    if ((int64_t)-lo * g >= 32769LL * BT_GAIN_ONE) {
        const int64_t q = (32769 * one2 - 1) / ((int64_t)-lo * g);
        if (q < r) r = q;
    }
    return (int32_t)r;
}

bool bt_level_block(bt_level_lim_t *l, int16_t *mono, const int16_t *stereo, size_t n, int32_t g, bool ahead)
{
    bool down = false;
    while (n) {
        const size_t k  = n > BT_LEVEL_AHEAD ? BT_LEVEL_AHEAD : n;
        int16_t     *in = l->held + BT_LEVEL_AHEAD;        /* these, after those held */
        for (size_t i = 0; i < k; i++) in[i] = (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) / 2);
        if (!ahead) {
            for (size_t i = 0; i < k; i++) mono[i] = (int16_t)((int64_t)in[i] * g / BT_GAIN_ONE);
            l->r = BT_GAIN_ONE;
        } else {
            /* The frames sent -- the k held longest -- and the
             * BT_LEVEL_AHEAD after them: the block's gain starts within its
             * own room and ends within the next one's too, so the next
             * block starts where this one ends. Back up a 32nd a block. */
            int32_t hi, lo;
            span(l->held, k, &hi, &lo);
            const int32_t now = share(hi, lo, g);
            span(l->held + k, BT_LEVEL_AHEAD, &hi, &lo);
            const int32_t next = share(hi, lo, g);
            const int32_t r0   = l->r < now ? l->r : now;
            int32_t       r1   = r0 + r0 / 32 + 1;
            if (r1 > now) r1 = now;
            if (r1 > next) r1 = next;
            /* The gain ramped from r0's share of g to r1's, both within
             * what is sent's room: every gain between them is too. With all
             * of it, g itself, sample for sample. */
            const int32_t g0 = (int32_t)((int64_t)g * r0 / BT_GAIN_ONE);
            const int32_t g1 = (int32_t)((int64_t)g * r1 / BT_GAIN_ONE);
            for (size_t i = 0; i < k; i++) {
                const int32_t gi = g0 + (g1 - g0) * (int32_t)(i + 1) / (int32_t)k;
                mono[i]          = (int16_t)((int64_t)l->held[i] * gi / BT_GAIN_ONE);
            }
            l->r = r1;
            down |= r0 < BT_GAIN_ONE || r1 < BT_GAIN_ONE;
        }
        memmove(l->held, l->held + k, BT_LEVEL_AHEAD * sizeof l->held[0]);
        mono   += k;
        stereo += 2 * k;
        n      -= k;
    }
    return down;
}

/* ---- the devices' levels ------------------------------------------------ */

static int find(const bt_levels_t *t, const uint8_t *bda)
{
    for (int i = 0; i < t->n; i++)
        if (!memcmp(t->rec[i].bda, bda, 6)) return i;
    return -1;
}

void bt_levels_load(bt_levels_t *t, const void *blob, size_t len)
{
    memset(t, 0, sizeof *t);
    const uint8_t *p = blob;
    for (size_t o = 0; p && o + sizeof(bt_level_rec_t) <= len && t->n < BT_LEVEL_DEVICES; o += sizeof(bt_level_rec_t)) {
        bt_level_rec_t r;
        memcpy(&r, p + o, sizeof r);
        if (bt_level_ok(r.db) && find(t, r.bda) < 0) t->rec[t->n++] = r;
    }
}

bool bt_levels_get(const bt_levels_t *t, const uint8_t bda[6], int *db)
{
    const int i = find(t, bda);
    if (i >= 0 && db) *db = t->rec[i].db;
    return i >= 0;
}

int bt_levels_level(const bt_levels_t *t, const uint8_t bda[6], uint8_t kind)
{
    int db;
    return bt_levels_get(t, bda, &db) ? db : bt_level_default(kind);
}

bool bt_levels_set(bt_levels_t *t, const uint8_t bda[6], int db)
{
    if (!bt_level_ok(db)) return false;
    int i = find(t, bda);
    if (i >= 0 && t->rec[i].db == db) return false;
    bt_level_rec_t r = { .db = (int8_t)db };
    memcpy(r.bda, bda, 6);
    if (i >= 0) r.spare = t->rec[i].spare;
    else        i = t->n < BT_LEVEL_DEVICES ? t->n++ : BT_LEVEL_DEVICES - 1;    /* all taken: the oldest goes */
    memmove(&t->rec[1], &t->rec[0], (size_t)i * sizeof t->rec[0]);
    t->rec[0] = r;
    return true;
}

bool bt_levels_forget(bt_levels_t *t, const uint8_t bda[6])
{
    const int i = find(t, bda);
    if (i < 0) return false;
    memmove(&t->rec[i], &t->rec[i + 1], (size_t)(t->n - i - 1) * sizeof t->rec[0]);
    t->n--;
    memset(&t->rec[t->n], 0, sizeof t->rec[0]);
    return true;
}
