/* TCI v2.0 client for AetherSDR.
 *
 * Connects out to ws://<host>:50001 and speaks the same protocol AetherSDR
 * already serves to WSJT-X. Design notes that matter:
 *
 *  - Tuning is OPTIMISTIC. The wire round trip is 30-80 ms with an unbounded
 *    tail (it waits on someone else's Qt event loop), while the glass and the
 *    motor are 10-20 ms. Neither may wait for the wire, so the display leads
 *    and the anti-echo classifier reconciles afterwards.
 *
 *  - There is NO outbound queue, deliberately. The server closes the socket
 *    after 64 queued commands, so a queue would turn an enthusiastic flick
 *    into a disconnection. Instead the sender polls for a difference and is
 *    hard-capped at 20 Hz, which makes that failure structurally impossible
 *    rather than merely unlikely.
 *
 *  - The greeting is authoritative on every (re)connect. Pushing our stale
 *    pre-dropout frequency at a rig the operator has since retuned would be
 *    the rudest possible bug.
 */
#ifndef TCI_CLIENT_H
#define TCI_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    TCI_LINK_DOWN = 0,
    TCI_LINK_CONNECTING,
    TCI_LINK_GREETING,
    TCI_LINK_READY,
    TCI_LINK_DEGRADED,   /* after a backlog close: send rate halved for 60 s */
} tci_link_t;

typedef struct {
    tci_link_t link;
    int64_t    f_display;      /* what the glass shows -- optimistic */
    int64_t    f_server;       /* newest authoritative value; never drawn */
    char       mode[8];
    int32_t    filt_lo, filt_hi;
    int32_t    rit_hz;
    float      smeter_dbm;
    bool       slice_locked;
    bool       tx;
    uint8_t    my_trx;
    uint8_t    n_trx;
    /* Counters for the status shade; all of these are diagnostics you want
     * when something feels wrong but nothing is obviously broken. */
    uint32_t   connects, closes, reconciles, rejects;
    uint32_t   unknown_cmds, sends, echoes;
    char       last_close[48];
} tci_status_t;

esp_err_t tci_client_start(const char *host, uint16_t port);

/* Called from the knob task. Moves f_display immediately; the wire catches up. */
void tci_tune_by(int32_t detents, uint8_t accel_mult, int32_t step_hz);

/* Adopt a step size (tapping a digit, later). Re-latches the anchor. */
void tci_set_step(int32_t step_hz);

void tci_get_status(tci_status_t *out);
bool tci_is_ready(void);

#endif /* TCI_CLIENT_H */
