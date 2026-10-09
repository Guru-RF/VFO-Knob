/* A WebSDR's audio, decoded. See wsdr_proto.h and WEBSDR-PROTOCOL.md §3.
 *
 * Each coded sample is a residual: a quotient in unary (an escape to eight
 * bits past 15 - I zeros), a mantissa of up to I - 1 bits -- fewer for large
 * quotients -- and a sign. The residual times the step, plus half a step,
 * corrects a prediction from the last twenty outputs, whose taps adapt with
 * every sample and leak back towards zero. AM and FM run the result through
 * an integrator as well. The arithmetic is the page's, which works in
 * JavaScript numbers: 32-bit wraps where it does bit operations, true halves
 * where it divides -- so the history is kept doubled here, and the sums in 64
 * bits, and every output is the browser's sample for sample. */
#include "wsdr_proto.h"

#include <string.h>

/* From these quotients on, by the coder's width, a mantissa bit less, and
 * another from the next (§3.4). */
static const int16_t SHRINK[8] = { 999, 999, 8, 4, 2, 1, 99, 99 };

/* ITU-T G.711 A-law to linear. */
static int16_t alaw(uint8_t c)
{
    const int a = c ^ 0x55;
    const int seg = (a >> 4) & 7;
    int t = (a & 0x0F) << 4;
    if (seg == 0) t += 8;
    else t = (t + 0x108) << (seg - 1);
    return (int16_t)((a & 0x80) ? t : -t);
}

/* JavaScript's ToInt32 and an Int16Array's store: the low bits, wrapped. */
static inline int32_t i32(int64_t v) { return (int32_t)(uint32_t)(uint64_t)v; }
static inline int16_t i16(int64_t v) { return (int16_t)(uint16_t)(uint64_t)v; }

static void forget(wsdr_dec_t *d)
{
    memset(d->tap, 0, sizeof d->tap);
    memset(d->hist2, 0, sizeof d->hist2);
    d->integ = 0;
}

void wsdr_dec_reset(wsdr_dec_t *d)
{
    memset(d, 0, sizeof *d);
    d->width = 1;
}

/* Big-endian, zeros past the end. */
static uint32_t at(const uint8_t *m, size_t n, size_t i) { return i < n ? m[i] : 0; }
static uint32_t be(const uint8_t *m, size_t n, size_t i, int k)
{
    uint32_t v = 0;
    while (k--) v = v << 8 | at(m, n, i++);
    return v;
}

/* One sample: its residual read from the stream at byte *pos, bit *bit, and
 * the predictor moved on by it. */
static int16_t sample(wsdr_dec_t *d, const uint8_t *m, size_t n, size_t *pos, int *bit)
{
    const int w = d->width;
    uint32_t win = be(m, n, *pos, 4) << *bit;

    /* The quotient: its zeros, then its 1 -- or, past 15 - I of them, its
     * eight bits. A window of nothing but zeros counts none, as the page's
     * loop does. */
    int zeros = 0, q;
    const int lim = 15 - w;
    if (win)
        while (!(win & 0x80000000u) && zeros < lim) {
            win <<= 1;
            zeros++;
        }
    int used;
    if (zeros < lim) {
        q = zeros;
        used = zeros + 1;
        win <<= 1;
    } else {
        q = (int)(win >> 24);
        used = zeros + 8;
        win <<= 8;
    }
    int shrunk = (q >= SHRINK[w]) + (q >= SHRINK[w - 1]);
    if (shrunk > w - 1) shrunk = w - 1;

    /* The mantissa's I - 1 - shrunk bits, standing for the top of I - 1; then
     * the sign: negative is the complement, of the shrunk-off bits set too. */
    int32_t res = (int32_t)((win >> 16 >> (17 - w)) & ~((1u << shrunk) - 1));   /* I = 1: none */
    res += q << (w - 1);
    if (win & (1u << (32 - w + shrunk))) res = ~(res | ((1 << shrunk) - 1));
    used += w - shrunk;
    *bit += used;
    *pos += (size_t)(*bit >> 3);
    *bit &= 7;

    /* The prediction: the taps over the history, in 1/4096, toward zero. */
    int64_t acc2 = 0;
    for (int i = 0; i < 20; i++) acc2 += (int64_t)d->tap[i] * d->hist2[i];
    int32_t pred = i32(acc2 / 2);
    pred = (pred >= 0 ? pred : pred + 4095) >> 12;

    /* The correction, residual x step + step / 2, doubled; the taps adapt by
     * it over the history, which then moves on. */
    const int64_t corr2 = 2 * (int64_t)res * d->step + d->step;
    const int32_t adapt = i32(corr2 / 2) >> 4;
    const int sh = (d->conv & 0x10) ? 12 : 14;
    for (int i = 19; i >= 0; i--) {
        d->tap[i] += -(d->tap[i] >> 7) + (i32(d->hist2[i] * adapt / 2) >> sh);
        if (i) d->hist2[i] = d->hist2[i - 1];
    }
    d->hist2[0] = 2 * (int64_t)pred + corr2;

    /* Out, with the integrator's sixteenth; which then takes this sample in,
     * or stays empty where the conversion says none. */
    const int16_t out = i16((d->hist2[0] + 2 * (int64_t)(i32(d->integ) >> 4)) / 2);
    if (d->conv & 0x10) d->integ = 0;
    else d->integ += (int32_t)((uint32_t)i32(d->hist2[0] / 2) << 4) >> 3;
    return out;
}

size_t wsdr_items(wsdr_dec_t *d, const uint8_t *m, size_t n, const wsdr_cb_t *cb)
{
    int16_t pcm[WSDR_BLOCK];
    size_t blocks = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = m[i];
        int bit;
        if (b >= 0xF0) {
            if (cb->meter) cb->meter(cb->ctx, (int)((b & 0x0F) << 8 | at(m, n, i + 1)));
            i += 1;
            continue;
        }
        if (b == 0x80) {
            for (int k = 0; k < WSDR_BLOCK; k++) pcm[k] = i + 1 + k < n ? alaw(m[i + 1 + k]) : 0;
            i += WSDR_BLOCK;
            forget(d);
            blocks++;
            if (cb->audio) cb->audio(cb->ctx, pcm, false);
            continue;
        }
        if (b >= 0x90 && b <= 0xDF) {
            d->width = (uint8_t)(14 - (b >> 4));
            bit = 4;                    /* its low nibble: the block's first bits */
        } else if (b < 0x80) {
            bit = 1;                    /* bit 7 the marker, the rest the block's */
        } else {
            switch (b) {
            case 0x81:
                if (cb->rate) cb->rate(cb->ctx, be(m, n, i + 1, 2));
                i += 2;
                break;
            case 0x82:
                d->step = be(m, n, i + 1, 2);
                i += 2;
                break;
            case 0x83:
                d->conv = (uint8_t)at(m, n, i + 1);
                i += 1;
                break;
            case 0x84:
                memset(pcm, 0, sizeof pcm);
                forget(d);
                blocks++;
                if (cb->audio) cb->audio(cb->ctx, pcm, true);
                break;
            case 0x85: {
                const uint32_t b1 = at(m, n, i + 1);
                const uint64_t mhz = (uint64_t)(b1 & 0x0F) << 40 | (uint64_t)be(m, n, i + 2, 1) << 32 |
                                     be(m, n, i + 3, 4);
                if (cb->carrier) cb->carrier(cb->ctx, mhz, (int)(b1 >> 4));
                i += 6;
                break;
            }
            case 0x86:
                if (cb->resync) cb->resync(cb->ctx);
                break;
            case 0x87:
                i += 6;
                break;
            default:
                break;                  /* 0x88-0x8F, 0xE0-0xEF: a byte passed by */
            }
            continue;
        }

        /* A coded block, from this byte's `bit`: its 128 samples, then on
         * from the next whole byte. */
        size_t pos = i;
        for (int k = 0; k < WSDR_BLOCK; k++) pcm[k] = sample(d, m, n, &pos, &bit);
        i = bit ? pos : pos - 1;
        blocks++;
        if (cb->audio) cb->audio(cb->ctx, pcm, false);
    }
    return blocks;
}
