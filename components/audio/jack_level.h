/* The jack's level: how much louder than the VOLUME it plays -- for a wired
 * headset that the VOLUME at 100 leaves too quiet (GitHub #12): the
 * PCM5100A is a line output, with no headphone amplifier behind it, and at
 * 100 it already plays the radio's audio as it comes. Plain C, with nothing
 * of ESP-IDF's: test/host tests it.
 *
 * From 0 to 12 dB, in steps of 3 dB, each the square root of two, as a
 * Bluetooth device's level (bt_link's bt_level.h). At 0 dB the jack plays
 * as it always did, sample for sample, with nothing held back. Above it a
 * loud passage could pass full scale, and is turned down instead, as a
 * Bluetooth device's is: the audio held back a block, JACK_AHEAD frames, to
 * see what comes, the gain ramped down through the block before a loud
 * passage so that its peak lands on full scale, never clipped, and back up
 * after it a 32nd a block. In stereo: both ears by the same gain, so the
 * radio's and a web SDR's balance stays as it was. */
#ifndef JACK_LEVEL_H
#define JACK_LEVEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JACK_LEVEL_MAX  12          /* dB */
#define JACK_LEVEL_STEP 3
#define JACK_GAIN_ONE   65536       /* a gain of 1: 0 dB */
#define JACK_AHEAD      240         /* frames looked ahead above 0 dB, and so held back:
                                       10 ms at 24 kHz, 15 at 16 */

/* A level the page may set: 0 to JACK_LEVEL_MAX, in steps. */
bool jack_level_ok(long db);

/* A level's gain: JACK_GAIN_ONE at 0 dB, four times it at +12. One between
 * the steps is taken at the nearest; past either end, at that end. */
int32_t jack_level_gain(int db);

/* The jack's audio between one block and the next: the frames held back,
 * and how far the gain is turned down. */
typedef struct {
    int16_t held[2 * 2 * JACK_AHEAD];   /* stereo: the last JACK_AHEAD frames, then the block coming in */
    int32_t r;                          /* the gain's share where the last block ended: JACK_GAIN_ONE, all */
} jack_lim_t;

/* Held back from now: silence, nothing turned down. */
void jack_lim_reset(jack_lim_t *l);

/* `n` frames of stereo, in place, at gain `g`: the n frames before them out,
 * held back since -- sample for sample while they and the JACK_AHEAD after
 * them are within full scale at `g`; else at less of it, ramped through the
 * block so that it ends within full scale for what comes next too, and back
 * up no faster than a 32nd a block. True when it turned the gain down. */
bool jack_lim_block(jack_lim_t *l, int16_t *stereo, size_t n, int32_t g);

#endif /* JACK_LEVEL_H */
