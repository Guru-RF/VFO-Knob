/* G.722 -- ITU-T Recommendation G.722, 7 kHz audio-coding within 64 kbit/s:
 * sub-band ADPCM, mode 1 only (64 kbit/s; the 56 and 48 kbit/s decoder
 * variants are not here). One octet for every two 16 kHz samples: bits 7-6
 * the higher sub-band's 2-bit code IH, bits 5-0 the lower sub-band's 6-bit
 * code IL -- the octet the Recommendation transmits, and what RTP payload
 * type 9 ("G722/8000") carries. 20 ms is 320 samples, 160 octets.
 * Written from the Recommendation (09/2012), clauses 5 and 6; g722.c says
 * how. 16 kHz mono int16 in and out; the state is the caller's. */
#ifndef G722_H
#define G722_H

#include <stdint.h>

/* One sub-band's ADPCM: the Recommendation's delayed variables (the ones it
 * resets), named as it names the lower sub-band's. */
typedef struct {
    int16_t al[2];      /* AL1, AL2: pole section coefficients */
    int16_t bl[6];      /* BL1..BL6: zero section coefficients */
    int16_t dlt[6];     /* DLT1..DLT6: quantized difference, delays 1 to 6 */
    int16_t plt[2];     /* PLT1, PLT2: partially reconstructed signal */
    int16_t rlt[2];     /* RLT1, RLT2: reconstructed signal */
    int16_t nbl;        /* NBL: logarithmic quantizer scale factor */
    int16_t detl;       /* DETL: quantizer scale factor */
} g722_sb_t;

typedef struct {
    int16_t    xin[24]; /* the transmit QMF's input, XIN..XIN23: newest first */
    g722_sb_t  lo, hi;
} g722_enc_t;

typedef struct {
    int16_t    xd[12];  /* the receive QMF's XD..XD11 (rL - rH), newest first */
    int16_t    xs[12];  /* and XS..XS11 (rL + rH) */
    g722_sb_t  lo, hi;
} g722_dec_t;

/* The Recommendation's reset: a call's (or a stream's) start. */
void g722_enc_init(g722_enc_t *s);
void g722_dec_init(g722_dec_t *s);

/* `samples` (even) at 16 kHz -> samples / 2 octets; returns the octets
 * written. An odd last sample is left out. */
int  g722_encode(g722_enc_t *s, const int16_t *pcm, int samples, uint8_t *out);

/* `bytes` octets -> 2 * bytes samples at 16 kHz; returns the samples
 * written. */
int  g722_decode(g722_dec_t *s, const uint8_t *in, int bytes, int16_t *pcm);

#endif /* G722_H */
