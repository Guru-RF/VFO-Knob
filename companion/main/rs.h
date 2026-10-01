/* A sample-rate converter between the knob's audio and the headset's: a
 * windowed sinc, in phases, whose ratio can be nudged -- the knob's clock and
 * the headset's are never quite the same, so the downlink is kept at its
 * target fill by running it a few parts per million fast or slow.
 *
 * One writer and one reader, on any two tasks: rs_push() moves only the
 * write index, rs_pull() and rs_drop() only the read one. */
#ifndef RS_H
#define RS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RS_TAPS   32           /* per phase: 0.7 ms at 24 kHz, the delay it adds */
#define RS_PHASES 64           /* interpolated between: plenty for speech */
#define RS_RING   8192         /* input samples held, a power of two: 340 ms at 24 kHz */

typedef struct {
    float    h[RS_PHASES + 1][RS_TAPS];
    int16_t  x[RS_RING + RS_TAPS];   /* the first RS_TAPS again at the end: a window never wraps */
    volatile uint32_t wr, rd;  /* input written; the window's first sample */
    double   frac;             /* the next output's place past the window's middle */
    double   step, nominal;    /* input samples per output */
    uint32_t in_rate, out_rate;
} rs_t;

/* Rates in Hz. Empties it. */
void rs_init(rs_t *r, uint32_t in_rate, uint32_t out_rate);
/* Input; what does not fit is dropped (the newest). Returns what was taken. */
size_t rs_push(rs_t *r, const int16_t *in, size_t n);
/* Input samples held, past the window. */
uint32_t rs_fill(const rs_t *r);
/* n outputs, or false -- and nothing taken -- without the input for them. */
bool rs_pull(rs_t *r, int16_t *out, size_t n);
/* As many outputs as the input allows, up to max. */
size_t rs_pull_some(rs_t *r, int16_t *out, size_t max);
/* Forget all but the newest `keep` input samples. */
void rs_drop(rs_t *r, uint32_t keep);
/* Run fast (+) or slow (-) by this fraction: 0.001 is 1000 ppm. */
void rs_nudge(rs_t *r, double frac);

#endif /* RS_H */
