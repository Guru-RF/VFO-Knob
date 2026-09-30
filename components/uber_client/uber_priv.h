/* Between the UberSDR client's two halves: the session (uber_client.c: the
 * audio socket, the dial) and the rest (uber_aux.c: spots, voice activity,
 * SSTV), which runs on its own task so that a download never holds up the
 * audio. */
#ifndef UBER_PRIV_H
#define UBER_PRIV_H

#include <stdbool.h>
#include <stdint.h>

#include "uber.h"
#include "uber_net.h"

extern uhost_t g_uh;                 /* the receiver, fixed from radio_start */

/* The session's UUID once /connection has taken it ("" before): the spots'
 * socket registers under it. `gen` moves on with each new one. */
bool uber_session_id(char *out, size_t cap, uint32_t *gen);

/* The receiver's clock in Unix seconds, from the audio's time stamps; 0
 * until the first. Spots are dated by it: the knob keeps no time of its own. */
int64_t uber_server_now(void);

/* The dial, as the spots are sorted and filtered against. */
void uber_dial(int64_t *hz, char *mode, size_t cap);

/* A band from the receiver's list (/api/bands), or the IARU's: its edges, or
 * false outside every band. */
bool uber_band_of(int64_t hz, int64_t *lo, int64_t *hi, char *label, size_t cap);

/* What a spot is to be tuned in, from its frequency and the cluster's
 * comment: "usb", "lsb", "cwu" -- or "" for a digital mode, not a voice. */
const char *uber_mode_for(uint32_t hz, const char *comment);

/* The spots' task: started with the session, going on while it does. */
void uber_aux_start(void);

#endif /* UBER_PRIV_H */
