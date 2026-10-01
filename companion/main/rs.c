/* The headset's sample-rate converter. See rs.h. */
#include "rs.h"

#include <math.h>
#include <string.h>

#define MASK (RS_RING - 1)

static double sinc(double x) { return x == 0.0 ? 1.0 : sin(M_PI * x) / (M_PI * x); }

void rs_init(rs_t *r, uint32_t in_rate, uint32_t out_rate)
{
    memset(r, 0, sizeof *r);
    r->in_rate  = in_rate;
    r->out_rate = out_rate;
    r->nominal  = r->step = (double)in_rate / (double)out_rate;
    /* Cut at 0.42 of the lower rate: 6.7 kHz between 16 and 24 kHz, 3.4 kHz
     * with an 8 kHz headset -- above anything a radio's filter passes. */
    const double fc   = 0.42 * (double)(in_rate < out_rate ? in_rate : out_rate) / (double)in_rate;
    const double half = RS_TAPS / 2;
    for (int p = 0; p <= RS_PHASES; p++) {
        const double f = (double)p / RS_PHASES;
        double sum = 0.0;
        for (int k = 0; k < RS_TAPS; k++) {
            const double d = (half - 1.0) + f - k;  /* the output's distance from tap k */
            const double u = d / half;              /* -1 .. 1 across the window */
            const double w = fabs(u) >= 1.0 ? 0.0
                           : 0.42 + 0.5 * cos(M_PI * u) + 0.08 * cos(2.0 * M_PI * u);  /* Blackman */
            const double v = 2.0 * fc * sinc(2.0 * fc * d) * w;
            r->h[p][k] = (float)v;
            sum += v;
        }
        for (int k = 0; k < RS_TAPS; k++) r->h[p][k] = (float)(r->h[p][k] / sum);   /* unity at DC */
    }
}

size_t rs_push(rs_t *r, const int16_t *in, size_t n)
{
    uint32_t wr = r->wr;
    const uint32_t rd = __atomic_load_n(&r->rd, __ATOMIC_ACQUIRE);
    const uint32_t room = RS_RING - (wr - rd);
    if (n > room) n = room;
    for (size_t i = 0; i < n; i++, wr++) {
        const uint32_t j = wr & MASK;
        r->x[j] = in[i];
        if (j < RS_TAPS) r->x[RS_RING + j] = in[i];
    }
    __atomic_store_n(&r->wr, wr, __ATOMIC_RELEASE);
    return n;
}

uint32_t rs_fill(const rs_t *r)
{
    return __atomic_load_n(&r->wr, __ATOMIC_ACQUIRE) - r->rd;
}

static inline float dot(const float *h, const int16_t *x)
{
    float a = 0.0f;
    for (int k = 0; k < RS_TAPS; k++) a += h[k] * (float)x[k];
    return a;
}

static inline int16_t one(rs_t *r, uint32_t *rd)
{
    const double ph = r->frac * RS_PHASES;
    const int    p  = (int)ph;
    const float  a  = (float)(ph - p);
    const int16_t *w = &r->x[*rd & MASK];
    float y = dot(r->h[p], w);
    if (a > 0.0f) y += a * (dot(r->h[p + 1], w) - y);
    r->frac += r->step;
    const uint32_t adv = (uint32_t)r->frac;
    r->frac -= adv;
    *rd += adv;
    return y > 32767.0f ? 32767 : y < -32768.0f ? -32768 : (int16_t)lrintf(y);
}

bool rs_pull(rs_t *r, int16_t *out, size_t n)
{
    const uint32_t wr = __atomic_load_n(&r->wr, __ATOMIC_ACQUIRE);
    uint32_t rd = r->rd;
    /* The last output's window starts this far in, and is RS_TAPS long. */
    const double last = r->frac + (double)(n ? n - 1 : 0) * r->step;
    if ((double)(wr - rd) < last + RS_TAPS) return false;
    for (size_t i = 0; i < n; i++) out[i] = one(r, &rd);
    __atomic_store_n(&r->rd, rd, __ATOMIC_RELEASE);
    return true;
}

size_t rs_pull_some(rs_t *r, int16_t *out, size_t max)
{
    const uint32_t wr = __atomic_load_n(&r->wr, __ATOMIC_ACQUIRE);
    uint32_t rd = r->rd;
    size_t n = 0;
    while (n < max && wr - rd >= RS_TAPS) out[n++] = one(r, &rd);
    __atomic_store_n(&r->rd, rd, __ATOMIC_RELEASE);
    return n;
}

void rs_drop(rs_t *r, uint32_t keep)
{
    const uint32_t wr = __atomic_load_n(&r->wr, __ATOMIC_ACQUIRE);
    if (wr - r->rd > keep) __atomic_store_n(&r->rd, wr - keep, __ATOMIC_RELEASE);
}

void rs_nudge(rs_t *r, double frac)
{
    r->step = r->nominal * (1.0 + frac);
}
