/* A Bluetooth headset's or speaker's level: how loud the knob sends it its
 * audio, on top of the knob's VOLUME -- one for each device, set on the
 * configuration page. Plain C, with nothing of ESP-IDF's: test/host tests
 * it.
 *
 * In 3 dB steps, from 24 dB less to 12 dB more: each step the square root
 * of two, so that 12 dB down is the quarter a speaker was always sent --
 * what the VOLUME gives the jack's earphones filled a room from the JLab at
 * 2 (2026-10-05). That is a speaker's level until one is set for it; a
 * headset's is 0 dB, the jack's level. A smaller speaker, the Sony
 * SRS-XB100, was far quieter than the JLab 12 dB down (2026-10-06): hence a
 * level for each device.
 *
 * Gains are fixed point, BT_GAIN_ONE for full scale. At 0 dB and below
 * nothing can pass full scale, and the audio goes to the device sample for
 * sample, as it always did. Above 0 dB a loud passage could: there the
 * audio is held back a block, BT_LEVEL_AHEAD frames, to see what comes, and
 * the gain is turned down ahead of a loud passage -- ramped through the
 * block before it, its peak at full scale, never clipped -- and back up
 * after it, a 32nd a block: it never jumps from one block to the next. A
 * block brought down whole by 32767 over its peak, as the web-SDR mixer's
 * tap does a strong signal's onset, clicked at its edges on CW at +12 dB:
 * the gain halved or doubled from one 10 ms to the next (2026-10-06).
 *
 * The levels set are kept for the last BT_LEVEL_DEVICES devices set, newest
 * first, by their Bluetooth address; bt_link keeps the records in NVS
 * (btlink/levels). */
#ifndef BT_LEVEL_H
#define BT_LEVEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BT_LEVEL_MIN     (-24)       /* dB */
#define BT_LEVEL_MAX     12
#define BT_LEVEL_STEP    3
#define BT_LEVEL_SPEAKER (-12)       /* a speaker's, until one is set: a quarter */
#define BT_LEVEL_HEADSET 0           /* a headset's: the jack's level */
#define BT_LEVEL_DEVICES 8           /* devices kept: the oldest set goes */

#define BT_GAIN_ONE      65536       /* a gain of 1: full scale, 0 dB */
#define BT_LEVEL_AHEAD   240         /* frames looked ahead above 0 dB, and so held back:
                                        the tap's block, 10 ms at 24 kHz, 15 at 16 */

/* A level the page may set: BT_LEVEL_MIN to BT_LEVEL_MAX, in steps. */
bool bt_level_ok(long db);

/* A device's level before one is set for it: a speaker's (bt_link_proto.h's
 * BTL_KIND_SPEAKER), or a headset's -- anything else is one. */
int bt_level_default(uint8_t kind);

/* A level's gain: BT_GAIN_ONE at 0 dB, a quarter of it at -12, four times it
 * at +12. A level between the steps is taken at the nearest, one past either
 * end at that end. */
int32_t bt_level_gain(int db);

/* What the device is sent, before the swell: the jack's loudness, the
 * knob's VOLUME (0-100) -- or, `full`, a speaker whose own volume is the
 * VOLUME (bt_link_proto.h's BTL_AV_SET): full scale, less in proportion what
 * it says it plays (`at`, 0-127; anything else unsaid) above the VOLUME
 * asked of it. A VOLUME of 0 is 0 either way. Then its level, `db`, on top. */
int32_t bt_level_want(uint8_t volume, bool full, uint8_t at, int db);

/* The gain sent, a block on, toward `want`: there at once -- unless it
 * swells, into full level: up a 32nd a block and a thousandth of full scale,
 * counted in thousandths as the tap always did -- some 27 dB a second at
 * 10 ms blocks, and block for block the swell it always was. The swell ends
 * at `want`, or with a want no higher than the gain. */
int32_t bt_level_swell(int32_t g, int32_t want, bool *swell);

/* A device's audio between one block and the next: the frames held back,
 * and how far the gain is turned down. */
typedef struct {
    int16_t held[2 * BT_LEVEL_AHEAD];   /* the last BT_LEVEL_AHEAD frames' means, then the block coming in */
    int32_t r;                          /* the gain's share where the last block ended: BT_GAIN_ONE, all */
} bt_level_lim_t;

/* A stream begins: nothing held back, nothing turned down. */
void bt_level_reset(bt_level_lim_t *l);

/* `n` frames of the jack's stereo, the mean of its two channels, at gain
 * `g`, into `mono`. Not `ahead` -- a level of 0 dB or less, whose gain no
 * sample passes full scale at -- these frames, sample for sample. `ahead`
 * -- a level above it -- the frames BT_LEVEL_AHEAD before these, held back
 * since: sample for sample while they and the next BT_LEVEL_AHEAD are
 * within full scale at `g`; else at less of it, ramped through the block so
 * that it ends within full scale for what comes next too, and back up no
 * faster than a 32nd a block. Turned on, the frames held back are sent
 * again; turned off, they are not sent: once, as the level crosses 0 dB.
 * True when it turned the gain down. */
bool bt_level_block(bt_level_lim_t *l, int16_t *mono, const int16_t *stereo, size_t n, int32_t g, bool ahead);

/* ---- the devices' levels ------------------------------------------------ */

/* One device's: written by one firmware and read by the next, so the
 * layout is frozen, and so are the eight at most. */
typedef struct __attribute__((packed)) {
    uint8_t bda[6];
    int8_t  db;
    uint8_t spare;                  /* 0, a later firmware's; kept as it was */
} bt_level_rec_t;
_Static_assert(sizeof(bt_level_rec_t) == 8, "frozen");

typedef struct {
    bt_level_rec_t rec[BT_LEVEL_DEVICES];   /* newest first */
    int            n;
} bt_levels_t;

/* From what NVS held: `len` bytes of records, newest first. One whose level
 * this firmware cannot take, or a device already read, is left out. */
void bt_levels_load(bt_levels_t *t, const void *blob, size_t len);

/* Its own level, if one is set for it: true, and *db. */
bool bt_levels_get(const bt_levels_t *t, const uint8_t bda[6], int *db);

/* The level it is sent at: its own, or its kind's default. */
int bt_levels_level(const bt_levels_t *t, const uint8_t bda[6], uint8_t kind);

/* Set for a device, and moved to the front; the oldest goes when all are
 * taken. True when the records changed: false for the same level again,
 * or one that is not bt_level_ok(). */
bool bt_levels_set(bt_levels_t *t, const uint8_t bda[6], int db);

/* Its level forgotten: true when it had one. */
bool bt_levels_forget(bt_levels_t *t, const uint8_t bda[6]);

#endif /* BT_LEVEL_H */
