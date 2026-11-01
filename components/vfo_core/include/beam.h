/* EpiBeam's directions (VFO_RADIO=epibeam): an array's beam steered to one of
 * 4 -- a 4-square's N, E, S and W -- or of 8, the corners between too. Only
 * arithmetic, for the face, the knob, the page and the host tests (test/host
 * test_beam). A direction is an index, 0 north and on round clockwise; the
 * motor counts one out as index + 1: N one, E two -- on 8, NE two. */
#ifndef VFO_BEAM_H
#define VFO_BEAM_H

#include <stdbool.h>
#include <stdint.h>

#define BEAM_MAX 8

/* 4 or 8: the arrays there are. */
bool        beam_dirs_ok(long n);

/* Its name, "N" to "NW"; "" past the array's directions. Anything but 8
 * directions is taken as 4. */
const char *beam_name(uint8_t n_dirs, uint8_t i);

/* Its heading, degrees clockwise from north. */
uint16_t    beam_deg(uint8_t n_dirs, uint8_t i);

/* `detents` on from i, round and round: clockwise for a turn clockwise. */
uint8_t     beam_step(uint8_t n_dirs, uint8_t i, int32_t detents);

/* The direction a point lies in, seen from the middle: dx to the right, dy
 * down, as the glass has them. Each owns the half-step either side of it. */
uint8_t     beam_at(uint8_t n_dirs, int32_t dx, int32_t dy);

/* Direction i of one array in another: the same heading where the other has
 * it, else the one before it, counter-clockwise -- NE on 4 is N. */
uint8_t     beam_convert(uint8_t from_n, uint8_t i, uint8_t to_n);

#endif /* VFO_BEAM_H */
