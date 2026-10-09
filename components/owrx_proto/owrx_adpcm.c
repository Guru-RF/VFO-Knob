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
 * 12 kHz), not the rest of the session.
 *
 * OpenWebRX 1.0 and 1.1 encode with csdr's older encode_ima_adpcm_i16_u8:
 * the same codec, the same nibble order, from index 0 and predictor 0 -- and
 * no SYNC ever. Their page decodes it as one stream from the session's
 * first byte to its last. A session's audio not starting with "SYNC" is
 * taken as that; a SYNC is still looked for over its first two blocks'
 * worth, in case it was SYNC-framed audio joined mid-block after all. */
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
#define PLAIN_WATCH (2 * (SYNC_EVERY + 8))   /* SYNC-framed audio shows one in each block */

enum { HUNT, HDR, DATA, PLAIN };

void owrx_adpcm_reset(owrx_adpcm_t *d)
{
    memset(d, 0, sizeof *d);
    d->st = HUNT;
}

void owrx_adpcm_resync(owrx_adpcm_t *d)
{
    d->odd = false;
    if (d->plain) return;
    d->st = HUNT;
    d->match = 0;
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

/* A SYNC's index and predictor, as they came: false if no SYNC after all,
 * its index past 88 ("SYNC" in the audio, by chance). */
static bool take(owrx_adpcm_t *d)
{
    const int16_t idx  = (int16_t)(d->hdr[0] | d->hdr[1] << 8);
    const int16_t pred = (int16_t)(d->hdr[2] | d->hdr[3] << 8);
    if (idx < 0 || idx > 88) return false;
    d->idx  = idx;
    d->pred = pred;
    d->left = SYNC_EVERY;
    d->st   = DATA;
    d->syncs++;
    return true;
}

/* Bytes gone into the state only: a handful of samples not played. */
static void into_state(owrx_adpcm_t *d, const uint8_t *p, size_t k)
{
    for (size_t i = 0; i < k; i++) {
        nib(d, p[i] & 0x0F);
        nib(d, p[i] >> 4);
    }
}

/* No SYNC where the session's audio starts: plain, from index 0 and
 * predictor 0. */
static void to_plain(owrx_adpcm_t *d)
{
    d->plain = true;
    d->st    = PLAIN;
    d->watch = PLAIN_WATCH;
    d->match = 0;
}

/* A plain byte, both its samples -- and while still watching, a SYNC looked
 * for in it: one with a sound index was SYNC-framed audio joined mid-block,
 * taken from there on as that. */
static size_t plain(owrx_adpcm_t *d, uint8_t b, int16_t *out)
{
    static const uint8_t SW[4] = { 'S', 'Y', 'N', 'C' };
    out[0] = nib(d, b & 0x0F);
    out[1] = nib(d, b >> 4);
    if (d->watch) {
        d->watch--;
        if (d->match == 4) {
            d->hdr[d->hn++] = b;
            if (d->hn == 4) {
                d->match = 0;
                if (take(d)) {
                    d->plain = false;
                    d->watch = 0;
                }
            }
        } else if (b == SW[d->match]) {
            if (++d->match == 4) d->hn = 0;
        } else {
            d->match = b == 'S';
        }
    }
    return 2;
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
            } else if (!d->syncs && !d->lost) {
                /* The session's first bytes, and no SYNC: plain. */
                into_state(d, SW, d->match);
                to_plain(d);
                o += plain(d, b, out + o);
            } else {
                /* Not it -- after as much of it as came, every byte passed
                 * over; this one may start it again. */
                d->lost += d->match + 1u - (b == 'S');
                d->match = b == 'S';
            }
            break;
        case HDR:
            d->hdr[d->hn++] = b;
            if (d->hn == 4 && !take(d)) {                /* "SYNC" in the audio, by chance */
                if (!d->syncs && !d->lost) {
                    into_state(d, SW, 4);
                    into_state(d, d->hdr, 4);
                    to_plain(d);
                    break;
                }
                d->lost += 8;
                d->st = HUNT;
                d->match = 0;
            }
            break;
        case PLAIN:
            o += plain(d, b, out + o);
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
