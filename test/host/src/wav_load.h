/* A WAV file into floats at a rate of one's choosing, for the host tools that
 * read recordings (test_tag.c, tag_wav.c). Straight-line resampling, as
 * TailChaser's reference does. */
#ifndef WAV_LOAD_H
#define WAV_LOAD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tag.h"

/* A 16-bit PCM WAV, its first channel, at TAG_FS. NULL without one. */
static float *load_wav(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(b); return NULL; }
    fclose(f);
    int ch = 0, rate = 0, bits = 0;
    const uint8_t *data = NULL;
    size_t dlen = 0;
    for (long at = 12; at + 8 <= len;) {
        const uint32_t sz = (uint32_t)b[at + 4] | (uint32_t)b[at + 5] << 8 | (uint32_t)b[at + 6] << 16 | (uint32_t)b[at + 7] << 24;
        if (!memcmp(b + at, "fmt ", 4)) {
            ch = b[at + 10] | b[at + 11] << 8;
            rate = (int)(b[at + 12] | b[at + 13] << 8 | b[at + 14] << 16 | (uint32_t)b[at + 15] << 24);
            bits = b[at + 22] | b[at + 23] << 8;
        } else if (!memcmp(b + at, "data", 4)) {
            data = b + at + 8;
            dlen = sz <= (uint32_t)(len - at - 8) ? sz : (size_t)(len - at - 8);
        }
        at += 8 + sz + (sz & 1);
    }
    if (!data || bits != 16 || ch < 1 || rate < 8000) { free(b); return NULL; }
    const size_t in = dlen / (2 * (size_t)ch);
    const size_t out = (size_t)((double)in * TAG_FS / rate);
    float *x = malloc(out * sizeof *x);
    for (size_t i = 0; i < out; i++) {
        const double t = (double)i * rate / TAG_FS;
        const size_t a = (size_t)t;
        const double fr = t - (double)a;
        const int16_t s0 = (int16_t)(data[2 * ch * a] | data[2 * ch * a + 1] << 8);
        const int16_t s1 = a + 1 < in ? (int16_t)(data[2 * ch * (a + 1)] | data[2 * ch * (a + 1) + 1] << 8) : s0;
        x[i] = (float)(((1 - fr) * s0 + fr * s1) / 32768.0);
    }
    free(b);
    *n = out;
    return x;
}

#endif
