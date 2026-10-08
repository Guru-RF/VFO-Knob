/* OpenWebRX's audio, decoded. See owrx_proto.h.
 *
 * The receiver's encoder is csdr's AdpcmEncoder(sync=True) (src/lib/
 * adpcm.cpp, the same in luarvique's csdr): Tim Kientzle's IMA ADPCM, two
 * samples a byte, the low nibble first, its state from index 0 and
 * predictor 0. Before its first byte, and then before every 1001st, it
 * writes "SYNC" and its step index and predictor as int16 little-endian --
 * where a frame happens to hold them, or across two. The decoder finds them
 * as they come, wherever the pieces break, and takes its state from each:
 * one lost frame costs the audio up to the next, 1001 bytes on (170 ms at
 * 12 kHz), not the rest of the session. */
#include "owrx_proto.h"

#include <string.h>

static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34,
    37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494,
    544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
    4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
};
static const int8_t ADJ[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

#define SYNC_EVERY 1001                 /* data bytes between two SYNCs */

enum { HUNT, HDR, DATA };

void owrx_adpcm_reset(owrx_adpcm_t *d)
{
    memset(d, 0, sizeof *d);
    d->st = HUNT;
}

/* csdr's AdpcmCodec::decodeSample, as it is. */
static int16_t nib(owrx_adpcm_t *d, uint8_t n)
{
    const int step = STEP[d->idx];
    int diff = step >> 3;
    if (n & 1) diff += step >> 2;
    if (n & 2) diff += step >> 1;
    if (n & 4) diff += step;
    if (n & 8) diff = -diff;
    d->pred += diff;
    if (d->pred > 32767) d->pred = 32767;
    else if (d->pred < -32768) d->pred = -32768;
    d->idx = (int16_t)(d->idx + ADJ[n]);
    if (d->idx < 0) d->idx = 0;
    else if (d->idx > 88) d->idx = 88;
    return (int16_t)d->pred;
}

size_t owrx_adpcm_feed(owrx_adpcm_t *d, const uint8_t *in, size_t n, int16_t *out)
{
    static const uint8_t SW[4] = { 'S', 'Y', 'N', 'C' };
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = in[i];
        switch (d->st) {
        case HUNT:
            if (b == SW[d->match]) {
                if (++d->match == 4) {
                    d->st = HDR;
                    d->hn = 0;
                }
            } else {
                /* Not it -- after as much of it as came, every byte passed
                 * over; this one may start it again. */
                d->lost += d->match + 1u - (b == 'S');
                d->match = b == 'S';
            }
            break;
        case HDR:
            d->hdr[d->hn++] = b;
            if (d->hn == 4) {
                const int16_t idx  = (int16_t)(d->hdr[0] | d->hdr[1] << 8);
                const int16_t pred = (int16_t)(d->hdr[2] | d->hdr[3] << 8);
                if (idx < 0 || idx > 88) {              /* "SYNC" in the audio, by chance */
                    d->lost += 8;
                    d->st = HUNT;
                    d->match = 0;
                    break;
                }
                d->idx  = idx;
                d->pred = pred;
                d->left = SYNC_EVERY;
                d->st   = DATA;
                d->syncs++;
            }
            break;
        default:
            out[o++] = nib(d, b & 0x0F);
            out[o++] = nib(d, b >> 4);
            if (--d->left == 0) {
                d->st = HUNT;
                d->match = 0;
            }
            break;
        }
    }
    return o;
}

size_t owrx_pcm_feed(owrx_adpcm_t *d, const uint8_t *in, size_t n, int16_t *out)
{
    size_t o = 0, i = 0;
    if (d->odd && n) {
        out[o++] = (int16_t)(d->lo | in[0] << 8);
        d->odd = false;
        i = 1;
    }
    for (; i + 1 < n; i += 2) out[o++] = (int16_t)(in[i] | in[i + 1] << 8);
    if (i < n) {
        d->lo  = in[i];
        d->odd = true;
    }
    return o;
}
