/* EpiBeam's link to EpiCenter, the station's brain: every connection goes
 * over a WebSocket to it, the array's switching its business. Not there yet:
 * for now the direction the knob settles on is only logged. */
#ifndef EPI_CLIENT_H
#define EPI_CLIENT_H

#include <stdint.h>

/* The beam on direction `dir` of an array of `n_dirs` (components/vfo_core
 * beam.h): settled on the dial, or tapped. */
void epi_beam_point(uint8_t dir, uint8_t n_dirs);

#endif /* EPI_CLIENT_H */
