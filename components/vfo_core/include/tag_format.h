/* TailChaser's callsign tag: the format, and nothing else.
 *
 * A TailChaser repeater adds a noise-like signal about 38 dB under its speech:
 * direct-sequence spread spectrum carrying the callsign of who is talking (the
 * node the over came from, or the repeater's own callsign with "local" set).
 * This firmware reads it from a radio's loudspeaker through the knob's
 * microphone (tag.c).
 *
 * FORMAT v0 -- A PROTOTYPE (2026-11-23). It may still change and has no version
 * field of its own, so every number of it lives here and only here. The
 * reference is TailChaser's tag2.py (Python); its vectors.json is checked
 * against this file by test/host/src/test_tag.c.
 *
 *   chips    2000/s, +1/-1, on a carrier of 4330 cycles per frame (1650.15 Hz),
 *            low-passed to about +-1150 Hz at baseband: ~500..2800 Hz
 *   symbol   32 chips = 16 ms, BPSK, differential: bit 1 = this symbol has the
 *            opposite sign of the one before
 *   frame    164 symbols = 2.624 s, back to back while the transmitter is on:
 *            symbol 0 the reference (+1), 1..15 the known sync bits, 16..163
 *            the 148 coded bits, interleaved. One chip sequence for the whole
 *            frame, the same in every frame.
 *   message  68 bits: callsign 48 (base 40, 9 characters, first character most
 *            significant), local 1, zero 3, CRC-16 16 -> K=7 rate 1/2
 *            convolutional code, 6 tail bits -> 148 coded bits
 */
#ifndef TAG_FORMAT_H
#define TAG_FORMAT_H

#define TAG_FORMAT          0           /* v0: the prototype */

#define TAG_CHIP_RATE       2000        /* chips per second */
#define TAG_CPS             32          /* chips per symbol */
#define TAG_NSYNC           16          /* symbol 0 (the reference) and 15 sync bits */
#define TAG_NBITS           148         /* coded bits per frame */
#define TAG_NSYM            (TAG_NSYNC + TAG_NBITS)          /* 164 */
#define TAG_FRAME_CHIPS     (TAG_NSYM * TAG_CPS)              /* 5248 */
#define TAG_CARRIER_CYCLES  4330        /* per frame: frames join without a step */
#define TAG_FC_HZ           ((double)TAG_CARRIER_CYCLES * TAG_CHIP_RATE / TAG_FRAME_CHIPS)
#define TAG_SHAPE_HZ        1150.0      /* the chips' low-pass at baseband */

/* Bit k (1..15) of this word is the sync bit of symbol k; bit 0 is unused. */
#define TAG_SYNC_WORD       0x9A6Du

/* The chips: a 17-bit LFSR from this seed, per chip
 *   bit = (r ^ r>>2 ^ r>>3 ^ r>>5) & 1;  r = (r >> 1) | (bit << 16);
 *   chip = (r & 1) ? +1 : -1                  -- the first 16: -+--+++--++-+-++ */
#define TAG_LFSR_SEED       0x1ACE5u

/* Coded bit i goes to position (i * TAG_PERM_MUL) % TAG_NBITS. */
#define TAG_PERM_MUL        59

#define TAG_CALL_LEN        9
#define TAG_ALPHABET        " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-/"   /* 0..38 */
#define TAG_CALL_BITS       48
#define TAG_LOCAL_BIT       48          /* 1: heard on the repeater's own input */
#define TAG_ZERO_BITS       3           /* 49..51, always 0 */
#define TAG_CRC_FROM        52          /* the CRC covers bits 0..51 */
#define TAG_PAYLOAD_BITS    68

#define TAG_CONV_K          7
#define TAG_CONV_G1         0171        /* octal */
#define TAG_CONV_G2         0133
#define TAG_CRC_POLY        0x1021u
#define TAG_CRC_INIT        0xFFFFu     /* MSB first, no final XOR */

/* Where it sits in the audio, for the level shown beside it. */
#define TAG_BAND_LO_HZ      500
#define TAG_BAND_HI_HZ      2800

#endif /* TAG_FORMAT_H */
