/* Reading TailChaser's callsign tag from microphone audio (tag.c).
 *
 * The format is tag_format.h's. The decoder takes the microphone at 16 kHz and
 * keeps the last ~8 s at 4000 complex samples a second around the carrier:
 *
 *   find    every lag is tried against the 16 known symbols, each symbol
 *           scaled by the level around it (so speech does not win), and what
 *           is a frame apart is added up: the frame's start stands out where
 *           the tag was heard in the clear, even in pieces.
 *   read    four times a second, at the best start, every symbol of the last
 *           one, two or three frames' time is read -- wherever it fell in its
 *           frame, so the quiet parts of different frames add up -- each
 *           weighted by how quiet it was around it; then the Viterbi and the
 *           CRC. A reading is a CRC that is right, its zero bits zero, and a
 *           callsign that is well formed. Nothing else is ever handed out.
 *   again   a sum belongs to one message, and a repeater changes the callsign
 *           only at a frame's edge (its frame clock runs on while it
 *           transmits): after a reading, the next is made only of what is
 *           heard after it. So it is one reading a frame or so, each of its
 *           own audio, and a new talker is never added to the last one.
 *
 * Nothing here knows ESP-IDF: the host tests (test/host/src/test_tag.c) run
 * it on TailChaser's recordings. */
#ifndef TAG_H
#define TAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tag_format.h"

#define TAG_FS          16000           /* the decoder's input rate */
#define TAG_DRIFT_PPM   120             /* the clocks' difference tried, each way */

/* --- the message ----------------------------------------------------------------- */

/* Upper case, up to 9 characters of TAG_ALPHABET, no space inside: false and
 * nothing written otherwise. */
bool     tag_call_ok(const char *call);
/* Whether a callsign looks like a radio amateur's: a prefix of 1..3 with a
 * letter, a digit, 1..4 letters (ON6URE, 2E0ABC, 3DA0RS); maybe a country's
 * prefix before a '/' (F/ON6URE) or 1..3 after one (ON6URE/P), maybe '-' and
 * one more at the end, as nodes are named (ON0ORA-L). Anything else is still
 * believed, after a second reading (tag_reading_t.sure). */
bool     tag_call_likely(const char *call);
bool     tag_pack(const char *call, bool local, uint8_t bits[TAG_PAYLOAD_BITS]);
/* The CRC, the zero bits and a well-formed callsign; `call` gets it without
 * its padding. */
bool     tag_unpack(const uint8_t bits[TAG_PAYLOAD_BITS], char call[TAG_CALL_LEN + 1], bool *local);
uint16_t tag_crc16(const uint8_t *bits, int n);
/* A callsign as a voice should say it: each character apart -- "O, N, six,
 * U, R, E" -- or (phonetic) in the ITU's words: "Oscar November Six Uniform
 * Romeo Echo"; '/' is "stroke", '-' "dash". The length written, 0 when it
 * does not fit in cap or the callsign is not one. */
size_t   tag_say_text(const char *call, bool phonetic, char *out, size_t cap);
void     tag_conv(const uint8_t in[TAG_PAYLOAD_BITS], uint8_t out[TAG_NBITS]);
/* soft[i] > 0: coded bit i is more likely 1. */
void     tag_viterbi(const float soft[TAG_NBITS], uint8_t out[TAG_PAYLOAD_BITS]);
int      tag_perm(int i);                         /* coded bit i's position */
int      tag_chip(int i);                         /* chip i of the frame: +1/-1 */
/* The frame's 164 symbol signs, +1/-1. */
bool     tag_frame_signs(const char *call, bool local, int8_t signs[TAG_NSYM]);

/* --- the decoder ------------------------------------------------------------------ */

typedef struct {
    char     call[TAG_CALL_LEN + 1];
    bool     local;
    uint64_t at;            /* the input sample (16 kHz) the reading was made at */
    float    sync_db;       /* how far the frame's start stood out */
    float    sync_avg;      /* its match over the last few frames, 0..1 */
    uint8_t  corrected;     /* coded bits the Viterbi turned round, of 148 */
    uint8_t  frames;        /* how many frames' time it took: 1..3 */
    int8_t   drift;         /* the clocks tried: 0, or the repeater's +-1 (TAG_DRIFT_PPM) */
    int8_t   nudge;         /* the start tried: 0 the best, or a sample either side */
    bool     sure;          /* believed at once: sync_avg >= TAG_SURE_AVG, one frame, tag_call_likely() */
} tag_reading_t;

typedef struct {
    float    band_dbfs;     /* the tag's band (about 400..2900 Hz), the last 256 ms; a full-scale sine there is 0 */
    float    all_dbfs;      /* everything, the last 256 ms */
    float    floor_dbfs;    /* the quietest 256 ms of the band in the last ~10 s */
    float    sync_db;       /* the best start against the rest, now */
    float    sync_now;      /* the best start's own match in the last frame, 0..1 */
    float    sync_avg;      /* ...and over the last few frames, 0..1 */
    bool     holding;       /* the last reading's start still stands out: its tag is on the air */
    uint32_t tries;         /* readings tried (the start stood out) */
    uint32_t readings;      /* ... that gave a callsign */
    uint32_t crc_bad;       /* ... where none of them did */
    uint64_t heard;         /* input samples taken */
} tag_stats_t;

typedef struct tag_dec tag_dec_t;

/* The decoder is one block of memory (~0.8 MB: the knob puts it in PSRAM). */
size_t   tag_dec_size(void);
void     tag_dec_init(tag_dec_t *d);
/* Microphone samples at TAG_FS. Returns how many readings are now waiting. */
int      tag_dec_feed(tag_dec_t *d, const int16_t *pcm, size_t n);
int      tag_dec_feed_f(tag_dec_t *d, const float *x, size_t n);   /* -1..1 */
bool     tag_dec_take(tag_dec_t *d, tag_reading_t *out);
void     tag_dec_stats(const tag_dec_t *d, tag_stats_t *st);

/* A reading is tried where the best start stands out this far from the rest,
 * and matches the known symbols at least this well. Speech alone can do both
 * now and then (up to 9.3 dB and 0.010 in TailChaser's recordings without a
 * tag): the CRC is what stops those. A reading of a start matching TAG_SURE_AVG
 * or better is sure -- every tail after an over gave 0.021 or more, speech
 * without a tag never 0.011 -- and a weaker one, read under speech, wants a
 * second reading of the same before it is believed (tag_hist.h).
 *
 * The CRC is 16 bits and a try decodes up to 27 times (three clocks, three
 * starts, one to three frames): about one try in 2400 that reads nothing could
 * pass it all the same. Off the air at the knob, one did (2026-11-23, three
 * frames of a quiet room and the start of the next tag: "UWYVJD6UE" at sync
 * 0.070). So a reading of more than one frame, or of something that does not
 * look like a callsign, waits for a second one too: noise never says the
 * same twice. */
#ifndef TAG_TRY_DB
#define TAG_TRY_DB   7.0f
#endif
#ifndef TAG_TRY_AVG
#define TAG_TRY_AVG  0.008f
#endif
/* ...and its start matched at least this well in the newest frame too: once
 * the tag is gone, the sum over the last frames takes ~13 s to die away, and
 * a reading tried every 256 ms meanwhile -- each ~120 ms of the knob's core
 * -- found nothing (on the knob, 2026-11-23). On the 210 channel-made files
 * it costs no reading and saves three tries in four. */
#ifndef TAG_TRY_NOW
#define TAG_TRY_NOW  0.008f
#endif
#define TAG_SURE_AVG 0.020f

#endif /* TAG_H */
